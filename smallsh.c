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
#include <errno.h>

#define MAX_COMMAND_LENGTH 2048
#define MAX_NUM_ARGS 512
#define MAX_NUM_CHILD_PROCESSES 500
#define CHANGE_DIRECTORY "cd"
#define STATUS "status"
#define EXIT "exit"
#define DEFAULT_IO "/dev/null"
#define PROMPT ": "
#define HOME "HOME"
#define EXITED_MESSAGE "exit value"
#define TERMINATED_MESSAGE "terminated by signal"
#define ENTER_FOREGROUND_MODE_MESSAGE "Entering foreground-only mode (& is now ignored)\n"
#define EXIT_FOREGROUND_MODE_MESSAGE "Exiting foreground-only mode\n"
#define ENTER_FOREGROUND_MODE_MESSAGE_LENGTH 50
#define EXIT_FOREGROUND_MODE_MESSAGE_LENGTH 30

typedef struct Command
{
    // Last value in args array passed to execvp must be null pointer
    char *args[MAX_NUM_ARGS + 1];
    char *inputFile;
    char *outputFile;
    bool runInBackground;
} Command;

// Global variables
static pid_t childProcesses[MAX_NUM_CHILD_PROCESSES];
static char statusMessage[25];
bool foregroundMode;

// Function prototypes
void registerParentSignalHandlers();
void registerChildSignalHandlers(Command *);
void handleSIGTSTP(int);
int getRawInput(char *);
void parseCommand(char *, Command *);
void cleanUpChildProcesses();
void changeDirectory(char *);
void printStatus();
void executeCommand(Command *);
void redirectIO(Command *);
void printDiagnosticArgsParsingResults(Command *);

int main(void)
{
    // Register signal handlers
    registerParentSignalHandlers();

    // Initialize default status message
    sprintf(statusMessage, "%s %d", EXITED_MESSAGE, 0);

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

        if (getRawInput(userInput))
        {
            // Restart loop if system call was interrupted by signal
            free(userInput);
            continue;
        }

        Command *command = calloc(1, sizeof(Command));

        parseCommand(userInput, command);

        // printDiagnosticArgsParsingResults(command);

        char *firstArg = command->args[0];

        // Handle exit command
        if (strcmp(firstArg, EXIT) == 0)
        {
            cleanUpChildProcesses();
            free(userInput);
            free(command);
            exit(0);
        }

        // Handle cd command
        else if (strcmp(firstArg, CHANGE_DIRECTORY) == 0)
        {
            changeDirectory(command->args[1]);
        }

        // Handle status command
        else if (strcmp(firstArg, STATUS) == 0)
        {
            printStatus();
        }

        // Execute non-empty, non-commented command
        else if (firstArg != NULL && command->args[0][0] != '#')
        {
            executeCommand(command);
        }

        free(userInput);
        free(command);
    }
    return 0;
}

void registerParentSignalHandlers()
{
    struct sigaction SIGTSTPAction, ignoreAction = {0};

    // Fill out the SIGTSTP_action struct
	// Register handleSIGTSTP as the signal handler
	SIGTSTPAction.sa_handler = handleSIGTSTP;
	// Block all catchable signals while handleSIGTSTP is running
	if (sigfillset(&SIGTSTPAction.sa_mask))
    {
        perror("sigfillset");
        exit(1);
    }
	// No flags set
	SIGTSTPAction.sa_flags = 0;

    // Install the signal handler
	if (sigaction(SIGTSTP, &SIGTSTPAction, NULL))
    {
        perror("sigaction");
        exit(1);
    }

    // Assign SIG_IGN as the struct's signal handler
    ignoreAction.sa_handler = SIG_IGN;

    // Register the ignoreAction as the handler for SIGINT
    if (sigaction(SIGINT, &ignoreAction, NULL))
    {
        perror("sigaction");
        exit(1);
    }
}

void registerChildSignalHandlers(Command *command)
{
    struct sigaction SIGINTAction, ignoreAction = {0};

    // Override SIGINT handler when run in foreground
    if (!command->runInBackground)
    {
        SIGINTAction.sa_handler = SIG_DFL;
        if (sigaction(SIGINT, &SIGINTAction, NULL))
        {
            perror("sigaction");
            exit(1);
        }
    }

    // Ignore SIGTSTP
    ignoreAction.sa_handler = SIG_IGN;
    if (sigaction(SIGTSTP, &ignoreAction, NULL))
    {
        perror("sigaction");
        exit(1);
    }
}

void handleSIGTSTP(int signo)
{
    char *message = foregroundMode ? EXIT_FOREGROUND_MODE_MESSAGE : ENTER_FOREGROUND_MODE_MESSAGE;
    int n = foregroundMode ? EXIT_FOREGROUND_MODE_MESSAGE_LENGTH : ENTER_FOREGROUND_MODE_MESSAGE_LENGTH;
    if (write(STDOUT_FILENO, message, n) == -1)
    {
        perror("write");
        exit(1);
    }
    // XOR to toggle bit
    foregroundMode ^= 1;
}

int getRawInput(char *buffer)
{
    if (!fgets(buffer, MAX_COMMAND_LENGTH + 1, stdin))
    {
        // Catch error due to interrupt signal
        if (errno == EINTR)
        {
            return 1;
        }
        perror("fgets");
        exit(1);
    }
    return 0;
}

void parseCommand(char *userInput, Command *command)
{
    // Strip newline character
    size_t n = strlen(userInput);
    if (userInput[n - 1] == '\n')
    {
        userInput[n - 1] = '\0';
    }

    // If last character is &, set the runInBackground flag and
    // strip it from input
    n = strlen(userInput);
    if (userInput[n - 1] == '&')
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
        if (strcmp(token, "<") == 0)
        {
            command->inputFile = strtok(NULL, " ");
        }

        // Redirect output
        else if (strcmp(token, ">") == 0)
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
        command->inputFile = command->inputFile ? command->inputFile : DEFAULT_IO;
        command->outputFile = command->outputFile ? command->outputFile : DEFAULT_IO;
    }
}

void cleanUpChildProcesses()
{
    // Clean up child processes
    for (pid_t i = 0; i < MAX_NUM_CHILD_PROCESSES; i++)
    {
        if (childProcesses[i] != 0)
        {
            // Ignore errors from zombie and recently terminated processes
            if (kill(childProcesses[i], SIGKILL) && errno != ESRCH)
            {
                perror("kill");
                exit(1);
            }
        }
    }
}

void changeDirectory(char *path)
{
    // Change to home directory if no path is specified
    if (path == NULL)
    {
        path = getenv(HOME);
    }

    if (chdir(path))
    {
        perror("chdir");
        exit(1);
    }
}

void printStatus()
{
    fprintf(stdout, "%s\n", statusMessage);
    fflush(stdout);
}

void executeCommand(Command *command)
{
    int childStatus;
    pid_t spawnPid = fork();
    pid_t childProcessIterator = 0;

    switch (spawnPid)
    {
    // On error
    case -1:
        perror("fork");
        exit(1);
        break;

    // In child process
    case 0:
        // Redirect I/O
        redirectIO(command);
        registerChildSignalHandlers(command);

        execvp(command->args[0], command->args);
        
        // Only executes on error
        perror("execvp");
        exit(1);
        break;
    // In parent process
    default:
        // Add child process to first empty array index
        while (childProcesses[childProcessIterator] != 0 && childProcessIterator < MAX_NUM_CHILD_PROCESSES)
        {
            childProcessIterator++;
        }
        childProcesses[childProcessIterator] = spawnPid;

        spawnPid = waitpid(spawnPid, &childStatus, command->runInBackground ? WNOHANG : 0);
        // printf("PARENT(%d): child(%d) terminated. Exiting\n", getpid(), spawnPid);


        if (WIFEXITED(childStatus))
        {
			// printf("Child %d exited normally with status %d\n", spawnPid, WEXITSTATUS(childStatus));
            sprintf(statusMessage, "%s %d", EXITED_MESSAGE, WEXITSTATUS(childStatus));
		}
        else
        {
			// printf("Child %d exited abnormally due to signal %d\n", spawnPid, WTERMSIG(childStatus));
            sprintf(statusMessage, "%s %d", TERMINATED_MESSAGE, WTERMSIG(childStatus));
            printStatus();
		}
        break;
    }
}

void redirectIO(Command *command)
{
    if (command->inputFile)
    {
        int inputFD = open(command->inputFile, O_RDONLY);
        if (inputFD == -1)
        {
            perror("open");
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
            perror("open");
            exit(1);
        }

        if (dup2(outputFD, STDOUT_FILENO) == -1)
        {
            perror("dup2");
            exit(1);
        }
    }
}

void printDiagnosticArgsParsingResults(Command *command)
{
    fprintf(stderr, "Parsing results:\n");
    fprintf(stderr, "command = %s\n", command->args[0]);
    fprintf(stderr, "args = {");
    for (int i = 0; i < MAX_NUM_ARGS; i++)
    {
        if (command->args[i] != NULL)
        {
            fprintf(stderr, "%s,", command->args[i]);
        }
    }
    fprintf(stderr, "}\n");
    fprintf(stderr, "inputFile = %s\n", command->inputFile);
    fprintf(stderr, "outputFile = %s\n", command->outputFile);
    fprintf(stderr, "runInBackground = %s\n", command->runInBackground ? "true" : "false");
}