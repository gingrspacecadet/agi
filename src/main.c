/*
  agi.c - minimal self-modifying single-binary scaffold
  - maps short-term memory file "stm.dat" (persisted across exec)
  - has a tiny learner (one weight + bias) stored in STM
  - contains an editable config block between BEGIN_CONFIG / END_CONFIG
  - every N iterations it mutates the config, runs "make", and execs the program
  - compile: gcc -O2 -Wall -o agi agi.c
  - run: ./agi
*/

/* ===== BEGIN_CONFIG
LEARNING_RATE=0.05
MUTATION_PROB=0.50
RECOMPILE_INTERVAL=10
  ===== END_CONFIG */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- short-term memory layout (persisted in a file via mmap) --- */
#define STM_PATH "stm.dat"
#define STM_SIZE 4096

typedef struct {
    uint64_t magic;
    uint64_t version;
    uint64_t iter;
    double weight; // simple single-weight learner
    double bias;
    double running_reward;
    char scratch[256];
} stm_t;

static stm_t* stm = NULL;

/* magic & version values to detect layout mismatch */
enum { STM_MAGIC = 0xA51A6F257F3C1BULL, STM_VERSION = 1 };

/* --- helper: read config block in this source file (parser for the BEGIN_CONFIG block) --- */
typedef struct {
    double learning_rate;
    double mutation_prob;
    int recompile_interval;
} config_t;

static config_t config_defaults = {0.05, 0.5, 10};

/* parse numeric value after 'KEY=' on a line, returns true if set */
static bool parse_config_line(const char* line, config_t* cfg) {
    if (strncmp(line, "LEARNING_RATE=", 14) == 0) {
        cfg->learning_rate = atof(line + 14);
        return true;
    } else if (strncmp(line, "MUTATION_PROB=", 13) == 0) {
        cfg->mutation_prob = atof(line + 13);
        return true;
    } else if (strncmp(line, "RECOMPILE_INTERVAL=", 19) == 0) {
        cfg->recompile_interval = atoi(line + 19);
        return true;
    }
    return false;
}

/* open this source file and find the config block, parse numbers */
static config_t read_config_from_source(const char* selfpath) {
    config_t cfg = config_defaults;
    FILE* f = fopen(selfpath, "r");
    if (!f)
        return cfg;
    char line[512];
    bool in_block = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "BEGIN_CONFIG")) {
            in_block = true;
            continue;
        }
        if (strstr(line, "END_CONFIG")) {
            in_block = false;
            break;
        }
        if (in_block) {
            // trim whitespace
            char* s = line;
            while (*s == ' ' || *s == '\t')
                s++;
            parse_config_line(s, &cfg);
        }
    }
    fclose(f);
    return cfg;
}

/* mutate the config block in-place: random tweaks */
static bool mutate_source_config(const char* selfpath, double mutation_prob) {
    FILE* in = fopen(selfpath, "r");
    if (!in)
        return false;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", selfpath, (int)getpid());
    FILE* out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        return false;
    }

    char line[512];
    bool in_block = false;
    while (fgets(line, sizeof(line), in)) {
        if (strstr(line, "BEGIN_CONFIG")) {
            in_block = true;
            fputs(line, out);
            /* write mutated config lines */
            // read original lines until END_CONFIG and discard them
            while (fgets(line, sizeof(line), in)) {
                if (strstr(line, "END_CONFIG"))
                    break;
            }
            // compute new values and write replacements
            double lr = config_defaults.learning_rate *
                        (1.0 + ((rand() / (double)RAND_MAX) - 0.5) * 0.5);
            if ((rand() / (double)RAND_MAX) < mutation_prob) {
                lr = lr * (1.0 + ((rand() / (double)RAND_MAX) - 0.5) * 0.5);
                if (lr <= 0)
                    lr = 0.001;
            }
            double mp = config_defaults.mutation_prob;
            if ((rand() / (double)RAND_MAX) < mutation_prob) {
                mp = fmin(0.99, fmax(0.01, mp + ((rand() / (double)RAND_MAX) - 0.5) * 0.2));
            }
            int ri = config_defaults.recompile_interval;
            if ((rand() / (double)RAND_MAX) < mutation_prob) {
                int delta = (rand() % 5) - 2;
                ri = fmax(1, ri + delta);
            }
            // Write new block lines
            fprintf(out, "LEARNING_RATE=%.6f\n", lr);
            fprintf(out, "MUTATION_PROB=%.6f\n", mp);
            fprintf(out, "RECOMPILE_INTERVAL=%d\n", ri);
            // write END_CONFIG marker
            fputs("  ===== END_CONFIG */\n", out); // note: maintain comment formatting
            // Now we must continue copying until end (we already wrote END_CONFIG; but source had
            // the trailing "*/") We continue reading the remainder of the original file and write
            // it out.
            while (fgets(line, sizeof(line), in)) {
                fputs(line, out);
            }
            // done
            break;
        } else {
            fputs(line, out);
        }
    }

    fclose(in);
    fclose(out);
    // atomically replace
    if (rename(tmp_path, selfpath) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

/* --- STM management --- */
static bool map_stm_file(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        perror("open(stm)");
        return false;
    }
    if (ftruncate(fd, STM_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        return false;
    }
    void* p = mmap(NULL, STM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return false;
    }
    stm = (stm_t*)p;
    // initialize if magic mismatch
    if (stm->magic != STM_MAGIC || stm->version != STM_VERSION) {
        memset(stm, 0, sizeof(stm_t));
        stm->magic = STM_MAGIC;
        stm->version = STM_VERSION;
        stm->iter = 0;
        stm->weight = 0.1; // small initial weight
        stm->bias = 0.0;
        stm->running_reward = 0.0;
        snprintf(stm->scratch, sizeof(stm->scratch), "fresh");
        msync(stm, sizeof(stm_t), MS_SYNC);
    }
    return true;
}

/* tiny forward: score = weight * x + bias */
static double forward(double x) {
    return stm->weight * x + stm->bias;
}

/* simple online update: delta rule */
static void update_weights(double x, double reward, double lr) {
    double pred = forward(x);
    double error = reward - pred;
    // gradient step: weight += lr * error * x
    stm->weight += lr * error * x;
    stm->bias += lr * error * 1.0;
    stm->running_reward = 0.99 * stm->running_reward + 0.01 * reward;
    msync(stm, sizeof(stm_t), MS_SYNC);
}

/* self-recompile: run "make" and then exec this program again */
static void recompile_and_exec(char* const argv0) {
    fprintf(stderr, "[agi] triggering recompile\n");
    pid_t pid = fork();
    if (pid == 0) {
        execlp("make", "make", "-B", NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[agi] make failed (status=%d)\n", WEXITSTATUS(status));
        return;
    }

    // ensure binary has +x
    if (chmod(argv0, 0755) != 0) {
        perror("[agi] chmod failed");
        return;
    }
    if (access(argv0, X_OK) != 0) {
        perror("[agi] binary not executable");
        return;
    }

    fprintf(stderr, "[agi] execing new binary %s\n", argv0);
    execl(argv0, argv0, NULL);
    perror("execv");
    exit(1);
}

/* utility: simple environment / toy task
   task: predict parity-ish function of iter -> reward is 1 if action sign matches desired value
*/
static double toy_environment_reward(int iter, double action_value) {
    // target function: reward if action_value sign matches ((iter % 10) < 5)
    int target = (iter % 10) < 5 ? 1 : -1;
    int act = (action_value >= 0) ? 1 : -1;
    return (act == target) ? 1.0 : -1.0;
}

/* main loop */
int main(int argc, char** argv) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    if (!map_stm_file(STM_PATH))
        return 1;
    char selfpath[4096] = {0};
    if (readlink("/proc/self/exe", selfpath, sizeof(selfpath) - 1) <= 0) {
        // fallback: argv[0]
        strncpy(selfpath, argv[0], sizeof(selfpath) - 1);
    }

    // read config from our own source file
    config_t cfg = read_config_from_source(selfpath);
    // (if parse failed, fall back to defaults)
    if (cfg.recompile_interval <= 0)
        cfg.recompile_interval = config_defaults.recompile_interval;

    fprintf(stderr, "[agi] start iter=%lu weight=%.6f bias=%.6f lr=%.4f mp=%.4f int=%d\n",
            stm->iter, stm->weight, stm->bias, cfg.learning_rate, cfg.mutation_prob,
            cfg.recompile_interval);

    for (;;) {
        // simple perception: scalar input is iter % 10 normalized
        int it = (int)stm->iter;
        double x = (double)(it % 10) - 4.5; // in approx [-4.5, 5.5]
        double out = forward(x);
        // act: sign of out
        double reward = toy_environment_reward(it, out);
        // learn online
        update_weights(x, reward, cfg.learning_rate);

        // write a human-readable scratch for observation
        snprintf(stm->scratch, sizeof(stm->scratch), "iter=%lu w=%.6f b=%.6f rr=%.4f", stm->iter,
                 stm->weight, stm->bias, stm->running_reward);
        msync(stm, sizeof(stm_t), MS_SYNC);

        if ((stm->iter % 100) == 0) {
            fprintf(stderr, "[agi] %s\n", stm->scratch);
        }

        // self-mod: occasionally mutate source then rebuild+exec
        if ((stm->iter > 0) && ((stm->iter % cfg.recompile_interval) == 0)) {
            double r = rand() / (double)RAND_MAX;
            if (r < cfg.mutation_prob) {
                fprintf(stderr, "[agi] mutating source (prob %.3f)\n", cfg.mutation_prob);
                if (mutate_source_config(selfpath, cfg.mutation_prob)) {
                    fprintf(stderr, "[agi] source mutated; recompiling\n");
                    recompile_and_exec(argv[0]);
                    // exec replaces process on success; if it returns, continue
                } else {
                    fprintf(stderr, "[agi] mutate_source_config failed\n");
                }
            } else {
                fprintf(stderr, "[agi] chose not to mutate this cycle (r=%.3f)\n", r);
            }
            // re-read config in case mutation didn't exec (or if no mutation)
            cfg = read_config_from_source(selfpath);
        }

        stm->iter++;
        usleep(100000); // 100ms tick
    }

    return 0;
}
