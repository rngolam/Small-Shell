#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <stddef.h>
#include <errno.h>

#define MAX_COMMAND_LENGTH 2048
#define MAX_NUM_ARGS 512
#define MAX_NUM_BACKGROUND_PROCESSES 500
#define CHANGE_DIRECTORY "cd"
#define STATUS "status"
#define EXIT "exit"
#define NULL_IO "/dev/null"
#define PROMPT ": "
#define EXPAND_VAR '$'
#define REDIRECT_INPUT "<"
#define REDIRECT_OUTPUT ">"
#define RUN_IN_BACKGROUND_CHAR '&'
#define HOME "HOME"
#define EXITED_MESSAGE "exit value"
#define TERMINATED_MESSAGE "terminated by signal"
#define ENTER_FOREGROUND_MODE_MESSAGE "Entering foreground-only mode (& is now ignored)"
#define EXIT_FOREGROUND_MODE_MESSAGE "Exiting foreground-only mode"
#define ENTER_FOREGROUND_MODE_MESSAGE_LENGTH 48
#define EXIT_FOREGROUND_MODE_MESSAGE_LENGTH 28

typedef struct Command
{
    // Last value in args array passed to execvp must be null pointer
    char *args[MAX_NUM_ARGS + 1];
    char *inputFile;
    char *outputFile;
    bool runInBackground;
} Command;

// Global variables
static pid_t backgroundProcesses[MAX_NUM_BACKGROUND_PROCESSES];
static int childStatus;
static volatile sig_atomic_t foregroundMode;

// Function prototypes
void registerParentSignalHandlers();
void registerChildSignalHandlers(Command *);
void handle_SIGTSTP(int);
int getInput(char *, pid_t);
void parseCommand(char *, Command *);
void cleanUpBackgroundProcesses();
void killBackgroundProcesses();
void changeDirectory(char *);
void printStatus();
void executeCommand(Command *);
void blockSIGTSTP(sigset_t *);
void unblockSIGTSTP(sigset_t *);
void redirectIO(Command *);

int main(void)
{
    // Register signal handlers
    registerParentSignalHandlers();

    pid_t pid = getpid();

    while (1)
    {
        // Print command prompt
        fprintf(stdout, PROMPT);
        fflush(stdout);

        char *userInput = malloc(sizeof(char) * MAX_COMMAND_LENGTH + 1);
        if (userInput == NULL)
        {
            perror("malloc");
            exit(1);
        }

        // Restart loop if getInput is interrupted
        if (getInput(userInput, pid))
        {
            free(userInput);
            continue;
        }

        Command *command = calloc(1, sizeof(Command));
        parseCommand(userInput, command);

        char *firstArg = command->args[0];

        // Handle blank lines and comments
        if (!firstArg || firstArg[0] == '#')
        {
            cleanUpBackgroundProcesses();
            free(userInput);
            free(command);
            continue;
        }

        // Handle exit command
        else if (!strcmp(firstArg, EXIT))
        {
            killBackgroundProcesses();
            free(userInput);
            free(command);
            break;
        }

        // Handle cd command
        else if (!strcmp(firstArg, CHANGE_DIRECTORY))
        {
            changeDirectory(command->args[1]);
        }

        // Handle status command
        else if (!strcmp(firstArg, STATUS))
        {
            printStatus();
        }

        // Execute non-built-in command
        else
        {
            executeCommand(command);
        }

        // Reap any terminated child processes before the next command prompt
        cleanUpBackgroundProcesses();

        free(userInput);
        free(command);
    }
    return 0;
}

/**
 * Registers signal handlers for the parent smallsh process and blocks catchable signals
 * while signal handlers are running.
 */
void registerParentSignalHandlers()
{
    struct sigaction SIGTSTP_action, ignore_action;
    memset(&SIGTSTP_action, 0, sizeof SIGTSTP_action);
    memset(&ignore_action, 0, sizeof ignore_action);

    // Fill out the SIGTSTP_action struct
    // Register handle_SIGTSTP as the signal handler
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    // Block all catchable signals while handle_SIGTSTP is running
    if (sigfillset(&SIGTSTP_action.sa_mask))
    {
        perror("sigfillset");
        exit(1);
    }
    // No flags set
    SIGTSTP_action.sa_flags = 0;

    // Install the signal handler
    if (sigaction(SIGTSTP, &SIGTSTP_action, NULL))
    {
        perror("sigaction");
        exit(1);
    }

    // Assign SIG_IGN as the struct's signal handler
    ignore_action.sa_handler = SIG_IGN;

    // Register the ignore_action as the handler for SIGINT
    if (sigaction(SIGINT, &ignore_action, NULL))
    {
        perror("sigaction");
        exit(1);
    }
}

/**
 * Registers signal handlers for forked child processes
 * @param command Pointer to the struct containing the parsed command line input
 */
void registerChildSignalHandlers(Command *command)
{
    struct sigaction SIGINT_action, ignore_action;
    memset(&SIGINT_action, 0, sizeof SIGINT_action);
    memset(&ignore_action, 0, sizeof ignore_action);

    // Override SIGINT handler when run in foreground
    if (!command->runInBackground)
    {
        SIGINT_action.sa_handler = SIG_DFL;
        if (sigaction(SIGINT, &SIGINT_action, NULL))
        {
            perror("sigaction");
            exit(1);
        }
    }

    // Ignore SIGTSTP
    ignore_action.sa_handler = SIG_IGN;
    if (sigaction(SIGTSTP, &ignore_action, NULL))
    {
        perror("sigaction");
        exit(1);
    }
}

/**
 * Handler function for SIGTSTP signals. Note that all catchable signals are blocked during execution.
 * @param signo The signal number
 */
void handle_SIGTSTP(__attribute__((unused)) int signo)
{
    char *message = foregroundMode ? EXIT_FOREGROUND_MODE_MESSAGE : ENTER_FOREGROUND_MODE_MESSAGE;
    int n = foregroundMode ? EXIT_FOREGROUND_MODE_MESSAGE_LENGTH : ENTER_FOREGROUND_MODE_MESSAGE_LENGTH;

    // XOR to toggle bit
    foregroundMode ^= 1;

    if (write(STDOUT_FILENO, "\n", 1) == -1)
    {
        perror("write");
        exit(1);
    }

    if (write(STDOUT_FILENO, message, n) == -1)
    {
        perror("write");
        exit(1);
    }

    if (write(STDOUT_FILENO, "\n", 1) == -1)
    {
        perror("write");
        exit(1);
    }
}

/**
 * Reads user input into a buffer and expands consecutive instances of EXPAND_VAR with the calling process' pid
 * @param buffer The buffer to write user input into
 * @param pid The calling process' pid
 * @return 0, or -1 on error, with errno set
 */
int getInput(char *buffer, pid_t pid)
{
    int offset = 0;
    char c1, c2;
    while ((c1 = fgetc(stdin)) != '\n')
    {
        // Catch error if interrupted
        if (c1 == EOF && errno == EINTR)
        {
            return -1;
        }

        // If two consecutive expand variables are read, write the pid to the buffer in place of them
        if (c1 == EXPAND_VAR)
        {
            c2 = fgetc(stdin);
            if (c2 == EXPAND_VAR)
            {
                int bytesWritten = sprintf(buffer + offset, "%d", pid);
                offset += bytesWritten;
                continue;
            }

            else
            {
                // If no expansion is needed, place second character back in the file stream
                ungetc(c2, stdin);
            }
        }
        buffer[offset] = c1;
        offset += 1;
    }
    // Null terminate the string
    buffer[offset] = '\0';

    return 0;
}

/**
 * Parses user input and populates fields in Command struct
 * @param userInput An allocated string containing the user's command line input
 * @param command Pointer to an allocated Command struct
 */
void parseCommand(char *userInput, Command *command)
{
    // If last character is &, set the runInBackground flag and
    // strip it from input
    size_t n = strlen(userInput);
    if (n && userInput[n - 1] == RUN_IN_BACKGROUND_CHAR)
    {
        if (!foregroundMode)
        {
            command->runInBackground = true;
        }
        userInput[n - 1] = '\0';
    }

    // Get command
    char *token;
    token = strtok(userInput, " ");
    command->args[0] = token;

    // Consume string and fill args array
    int i = 1;
    while ((token = strtok(NULL, " ")))
    {
        // Redirect input
        if (!strcmp(token, REDIRECT_INPUT))
        {
            command->inputFile = strtok(NULL, " ");
        }

        // Redirect output
        else if (!strcmp(token, REDIRECT_OUTPUT))
        {
            command->outputFile = strtok(NULL, " ");
        }

        else
        {
            command->args[i] = token;
            i++;
        }
    }

    // Set default I/O path for background process if necessary
    if (command->runInBackground)
    {
        command->inputFile = command->inputFile ? command->inputFile : NULL_IO;
        command->outputFile = command->outputFile ? command->outputFile : NULL_IO;
    }
}

/**
 * Reaps any zombie background processes that have terminated
 * and evicts them the background processes array
 */
void cleanUpBackgroundProcesses()
{
    pid_t childPid;

    // Reap child processes that have terminated
    while ((childPid = waitpid(-1, &childStatus, WNOHANG)) > 0)
    {
        fprintf(stdout, "background pid %d is done: ", childPid);
        printStatus();
        fflush(stdout);

        // Search for terminated childPid and remove it from array
        for (pid_t i = 0; i < MAX_NUM_BACKGROUND_PROCESSES; i++)
        {
            if (backgroundProcesses[i] == childPid)
            {
                backgroundProcesses[i] = 0;
                break;
            }
        }
    }
}

/**
 * Sends a SIGKILL signal to all background processes
 */
void killBackgroundProcesses()
{
    for (pid_t i = 0; i < MAX_NUM_BACKGROUND_PROCESSES; i++)
    {
        if (backgroundProcesses[i])
        {
            // Ignore errors from zombie and recently terminated processes
            if (kill(backgroundProcesses[i], SIGKILL) && errno != ESRCH)
            {
                perror("kill");
                exit(1);
            }
        }
    }
}

/**
 * Changes the working directory to the specified path
 * @param path A string containing the relative or absolute path the user
 * wishes to change the working directory to. If NULL, changes to the directory to that
 * specified in the HOME environment variable
 */
void changeDirectory(char *path)
{
    // Change to home directory if no path is specified
    if (!path)
    {
        path = getenv(HOME);
    }

    if (chdir(path))
    {
        perror("chdir");
    }
}

/**
 * Displays information about the most recently ended child process
 */
void printStatus()
{
    if (WIFEXITED(childStatus))
    {
        fprintf(stdout, "%s %d\n", EXITED_MESSAGE, WEXITSTATUS(childStatus));
    }
    else
    {
        fprintf(stdout, "%s %d\n", TERMINATED_MESSAGE, WTERMSIG(childStatus));
    }
    fflush(stdout);
}

/**
 * Executes a non-built-in shell command specified in the Command struct by searching for the executable
 * in the PATH environmental variable. Forks the process and either waits for the child process to terminate (if run in
 * the foreground) or immediately returns control of the terminal to the user (if run in the background).
 * @param command Pointer to the struct containing the parsed command line input
 */
void executeCommand(Command *command)
{
    pid_t spawnPid = fork();
    pid_t backgroundProcessIterator = 0;

    sigset_t block_mask;

    switch (spawnPid)
    {
    // On error
    case -1:
        perror("fork");
        exit(1);
        break;

    // In child process
    case 0:
        redirectIO(command);
        registerChildSignalHandlers(command);

        execvp(command->args[0], command->args);

        // Only executes on error
        fprintf(stderr, "%s: %s\n", command->args[0], strerror(errno));
        exit(1);
        break;

    // In parent process
    default:
        // Temporarily block SIGTSTP signals
        blockSIGTSTP(&block_mask);

        // Store information about background process
        if (command->runInBackground)
        {
            // Add background process to first empty array index
            while (backgroundProcesses[backgroundProcessIterator] && backgroundProcessIterator < MAX_NUM_BACKGROUND_PROCESSES)
            {
                backgroundProcessIterator++;
            }
            // Kill background processes if there is no more room in array
            if (backgroundProcessIterator == MAX_NUM_BACKGROUND_PROCESSES)
            {
                killBackgroundProcesses();
                backgroundProcessIterator = 0;
            }

            backgroundProcesses[backgroundProcessIterator] = spawnPid;

            fprintf(stdout, "background pid is %d\n", spawnPid);
            fflush(stdout);
        }

        // If process is run in foreground, no flags are set and execution will hang until the specified pid terminates
        // If run in the background, the WNOHANG flag is set and waitpid immediately returns 0
        spawnPid = waitpid(spawnPid, &childStatus, command->runInBackground ? WNOHANG : 0);

        // Immediately print out status message for foreground processes killed by a signal
        if (!command->runInBackground && WIFSIGNALED(childStatus))
        {
            printStatus();
        }

        // Unblock SIGTSTP signals (any signals blocked during foreground process will be processed by handler)
        unblockSIGTSTP(&block_mask);
        break;
    }
}

/**
 * Adds the SIGTSTP signal to a signal set and changes calling thread's signal mask to block these signals
 * @param block_mask Pointer a signal set
 */
void blockSIGTSTP(sigset_t *block_mask)
{
    if (sigemptyset(block_mask))
    {
        perror("sigemptyset");
        exit(1);
    }

    if (sigaddset(block_mask, SIGTSTP))
    {
        perror("sigaddset");
        exit(1);
    }

    if (sigprocmask(SIG_BLOCK, block_mask, NULL))
    {
        perror("sigprocmask");
        exit(1);
    }
}

/**
 * Removes the SIGTSTP signal from a signal set and changes calling thread's signal mask to unblock these signals
 * @param block_mask Pointer a signal set
 */
void unblockSIGTSTP(sigset_t *block_mask)
{
    if (sigprocmask(SIG_UNBLOCK, block_mask, NULL))
    {
        perror("sigprocmask");
        exit(1);
    }
}

/**
 * Redirects input/output from stdin/stdout to the input/output files specified in the Command struct
 * @param command Pointer to the struct containing the parsed command line input
 */
void redirectIO(Command *command)
{
    if (command->inputFile)
    {
        int inputFD = open(command->inputFile, O_RDONLY);
        if (inputFD == -1)
        {
            fprintf(stderr, "cannot open %s for input\n", command->inputFile);
            exit(1);
        }

        if (dup2(inputFD, STDIN_FILENO) == -1)
        {
            perror("dup2");
            exit(1);
        }
    }

    if (command->outputFile)
    {
        int outputFD = open(command->outputFile, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP);
        if (outputFD == -1)
        {
            fprintf(stderr, "cannot open %s for output\n", command->outputFile);
            exit(1);
        }

        if (dup2(outputFD, STDOUT_FILENO) == -1)
        {
            perror("dup2");
            exit(1);
        }
    }
}