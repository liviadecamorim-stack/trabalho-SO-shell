#define _GNU_SOURCE
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/types.h"
#include "sys/wait.h"
#include "fcntl.h"

#define MAX_PATHS 128

// Estrutura global para gerenciar os caminhos de busca
char *paths[MAX_PATHS];
int path_count = 0;

// Imprime a mensagem de erro padrão estipulada nas especificações
void print_error(void) {
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
}

// Remove espaços em branco (espaços e tabs) do início e do fim da string
char *trim_whitespace(char *str) {
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return str;
}

// Inicializa a variável path com /bin
void init_path(void) {
    for (int i = 0; i < MAX_PATHS; i++) paths[i] = NULL;
    paths[0] = strdup("/bin");
    path_count = 1;
}

// Atualiza a variável path (comando interno 'path')
void set_path(char **args) {
    // Libera os caminhos anteriores
    for (int i = 0; i < path_count; i++) {
        free(paths[i]);
        paths[i] = NULL;
    }
    path_count = 0;

    int i = 1;
    while (args[i] != NULL && path_count < MAX_PATHS - 1) {
        paths[path_count++] = strdup(args[i]);
        i++;
    }
}

// Encontra o caminho executável válido usando access()
char *find_executable(char *cmd) {
    if (cmd == NULL || strlen(cmd) == 0) return NULL;

    for (int i = 0; i < path_count; i++) {
        char *full_path = malloc(strlen(paths[i]) + strlen(cmd) + 2);
        if (!full_path) continue;

        sprintf(full_path, "%s/%s", paths[i], cmd);
        if (access(full_path, X_OK) == 0) {
            return full_path;
        }
        free(full_path);
    }
    return NULL;
}

// Processa e executa um único comando (pode incluir redirecionamento)
pid_t execute_single_command(char *cmd_str) {
    cmd_str = trim_whitespace(cmd_str);
    if (strlen(cmd_str) == 0) return -1;

    // Trata Redirecionamento '>'
    char *redir_pos = strchr(cmd_str, '>');
    char *output_file = NULL;

    if (redir_pos != NULL) {
        *redir_pos = '\0'; // Separa o comando do arquivo de saída
        char *redir_target = redir_pos + 1;

        // Verifica se há múltiplos '>' na mesma instrução
        if (strchr(redir_target, '>') != NULL) {
            print_error();
            return -1;
        }

        redir_target = trim_whitespace(redir_target);
        
        // Deve haver exatamente um arquivo após o '>' (sem espaços no meio do nome)
        if (strlen(redir_target) == 0) {
            print_error();
            return -1;
        }

        // Tokeniza o lado direito para verificar se há mais de um token/arquivo
        char *saved_ptr;
        char *file_token = strtok_r(redir_target, " \t\n\r", &saved_ptr);
        if (file_token == NULL || strtok_r(NULL, " \t\n\r", &saved_ptr) != NULL) {
            print_error();
            return -1;
        }
        output_file = file_token;
    }

    // Tokeniza os argumentos do comando
    char *args[128];
    int arg_count = 0;
    char *token;
    char *rest = cmd_str;

    while ((token = strsep(&rest, " \t")) != NULL) {
        if (*token != '\0') {
            args[arg_count++] = token;
        }
    }
    args[arg_count] = NULL;

    if (arg_count == 0) {
        if (redir_pos != NULL) print_error(); // Redirecionamento sem comando prévio
        return -1;
    }

    // --- Tratamento de Comandos Embutidos (Built-in Commands) ---
    if (strcmp(args[0], "exit") == 0) {
        if (arg_count != 1) {
            print_error();
        } else {
            exit(0);
        }
        return -1;
    } 
    else if (strcmp(args[0], "cd") == 0) {
        if (arg_count != 2) {
            print_error();
        } else if (chdir(args[1]) != 0) {
            print_error();
        }
        return -1;
    } 
    else if (strcmp(args[0], "path") == 0) {
        set_path(args);
        return -1;
    }

    // --- Execução de Programas Externos ---
    char *executable = find_executable(args[0]);
    if (executable == NULL) {
        print_error();
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        print_error();
        free(executable);
        return -1;
    } 
    else if (pid == 0) {
        // Processo Filho
        if (output_file != NULL) {
            int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                print_error();
                exit(1);
            }
            // Redireciona stdout e stderr para o arquivo destino
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        execv(executable, args);
        // Se execv retornar, é porque falhou
        print_error();
        exit(1);
    }

    free(executable);
    return pid; // Retorna PID para controle de concorrência paralela
}

// Avalia a linha digitada e trata a execução em paralelo ('&')
void parse_and_execute_line(char *line) {
    char *cmd;
    char *rest = line;
    pid_t pids[128];
    int pid_count = 0;

    // Divide os comandos separados pelo operador paralelo '&'
    while ((cmd = strsep(&rest, "&")) != NULL) {
        pid_t pid = execute_single_command(cmd);
        if (pid > 0) {
            pids[pid_count++] = pid;
        }
    }

    // Aguarda todos os processos paralelos disparados na linha finalizarem
    for (int i = 0; i < pid_count; i++) {
        waitpid(pids[i], NULL, 0);
    }
}

int main(int argc, char *argv[]) {
    FILE *input_stream = stdin;
    int interactive_mode = 1;

    // Validação dos argumentos da linha de comando
    if (argc > 2) {
        print_error();
        exit(1);
    } 
    else if (argc == 2) {
        // Modo Batch
        interactive_mode = 0;
        input_stream = fopen(argv[1], "r");
        if (input_stream == NULL) {
            print_error();
            exit(1);
        }
    }

    init_path();

    char *line = NULL;
    size_t len = 0;

    while (1) {
        if (interactive_mode) {
            printf("wish> ");
            fflush(stdout);
        }

        ssize_t read = getline(&line, &len, input_stream);
        
        // Fim de arquivo (EOF)
        if (read == -1) {
            free(line);
            exit(0);
        }

        parse_and_execute_line(line);
    }

    free(line);
    return 0;
}