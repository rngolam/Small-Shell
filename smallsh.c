#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_COMMAND_LENGTH 2048
#define MAX_NUM_ARGS 512
#define CHANGE_DIRECTORY "cd"
#define STATUS "status"
#define EXIT "exit"
#define DEFAULT_IO "/dev/null"
#define PROMPT ": "
#define HOME "HOME"

typedef struct
{
    // Last value in args array passed to execvp must be null pointer
    char *args[MAX_NUM_ARGS + 1];
    char *inputFile;
    char *outputFile;
    _Bool runInBackground;
} Command;

// Function prototypes
void getRawInput(char *);
void parseCommand(char *, Command *);
void changeDirectory(char *);
void executeCommand(Command*);
void printDiagnosticArgsParsingResults(Command *);

int main(void)
{
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

        getRawInput(userInput);
        
        Command *command = calloc(1, sizeof(Command));

        parseCommand(userInput, command);

        // Handle blank lines and comments
        if (command->args[0] == NULL || command->args[0][0] == '#')
        {
            free(userInput);
            free(command);
            continue;
        }
        // printDiagnosticArgsParsingResults(command);

        // Handle cd
        else if (strcmp(command->args[0], CHANGE_DIRECTORY) == 0)
        {
            changeDirectory(command->args[1]);
        }

        else
        {
            executeCommand(command);
        }

        free(userInput);
        free(command);

    }
    return 0;
}

void getRawInput(char *buffer)
{
    fgets(buffer, MAX_COMMAND_LENGTH + 1, stdin);
}

void parseCommand(char *userInput, Command *command)
{
    // Strip newline character
    size_t n = strlen(userInput);
    if (userInput[n - 1] == '\n')
    {
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

        // Flag to run process in background
        else if (strcmp(token, "&") == 0)
        {
            command->runInBackground = 1;
        }

        else
        {
            command->args[i] = token;
            i++;
        }
    }
}

void changeDirectory(char *path)
{
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

void executeCommand(Command *command)
{
    int childStatus;
    pid_t spawnPid = fork();

    switch(spawnPid)
    {
        // On error
        case -1:
            perror("fork");
            exit(1);
            break;

        // In child process
        case 0:
            execvp(command->args[0], command->args);
            perror("execv");
            exit(1);
            break;
        // In parent process
        default:
            spawnPid = waitpid(spawnPid, &childStatus, command->runInBackground ? WNOHANG : 0);
            // printf("PARENT(%d): child(%d) terminated. Exiting\n", getpid(), spawnPid);
            break;
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