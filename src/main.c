#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// BEGIN CONFIG

// END CONFIG

// --- basic memory handles ---
#define STM_SIZE 4096 // short-term memory size

typedef struct {
    int counter;     // example: track iterations
    char notes[256]; // scratchpad
} STM;

STM* short_term;

// map short-term memory into a file (survives exec)
STM* map_stm(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    ftruncate(fd, STM_SIZE);
    STM* p = mmap(NULL, STM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p;
}

// --- AI core skeleton ---
typedef struct {
    char msg[128];
} Input;

typedef struct {
    char thought[128];
} Thought;

typedef struct {
    char action[128];
} Action;

Input perceive(void) {
    Input in;
    snprintf(in.msg, sizeof(in.msg), "tick %d", short_term->counter);
    return in;
}

Thought reason(Input in) {
    Thought t;
    snprintf(t.thought, sizeof(t.thought), "I perceived: %s", in.msg);
    return t;
}

Action decide(Thought t) {
    Action a;
    snprintf(a.action, sizeof(a.action), "recorded '%s'", t.thought);
    return a;
}

void perform(Action a) {
    // write into STM (persists across restarts)
    snprintf(short_term->notes, sizeof(short_term->notes), "%s", a.action);
    short_term->counter++;
}

// --- self-modification (stub) ---
bool should_self_modify(void) {
    return (short_term->counter > 0 && short_term->counter % 5 == 0);
}

void recompile_and_exec(const char* self) {
    printf("Recompiling...\n");
    pid_t pid = fork();
    if (pid == 0) {
        // child: run make
        execlp("make", "make", NULL);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);

    printf("Restarting new binary...\n");
    execl(self, self, NULL); // replace with new self
    _exit(1);
}

void mutate_source(const char* path) {
    FILE* in = fopen(path, "r");
    FILE* out = fopen("tmp.c", "w");

    char line[256];
    bool in_config = false;
    while (fgets(line, sizeof(line), in)) {
        if (strstr(line, "// BEGIN CONFIG")) {
            in_config = true;
            fprintf(out, "// BEGIN CONFIG\n");
            // Write new values
            fprintf(out, "#define LEARNING_RATE %.3f\n", drand48());
            fprintf(out, "#define EXPLORATION %d\n", rand() % 10);
            continue;
        }
        if (strstr(line, "// END CONFIG")) {
            in_config = false;
            fprintf(out, "// END CONFIG\n");
            continue;
        }
        if (!in_config) {
            fputs(line, out); // copy unchanged
        }
    }

    fclose(in);
    fclose(out);
    rename("tmp.c", path); // replace old source
}

// --- main loop ---
int main(int argc, char** argv) {
    short_term = map_stm("stm.dat");

    while (1) {
        Input in = perceive();
        Thought t = reason(in);
        Action a = decide(t);
        perform(a);

        printf("Iteration %d: %s\n", short_term->counter, short_term->notes);

        if (should_self_modify()) {
            mutate_source("../src/main.c");
            recompile_and_exec(argv[0]);
        }

        sleep(1);
    }
}
