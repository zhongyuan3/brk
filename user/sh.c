#include "ulib.h"

#define CWD_BUF_SIZE 1024
#define INPUT_BUF_SIZE 1024
#define CMD_ARGS_MAX 20

#define WHITESPACE " \t\n\r\v"

enum tok_type {
	TOK_ILLEGAL,
	TOK_EOF,
	TOK_ARG,
	TOK_PIPE,
	TOK_REDIR_IN,
	TOK_REDIR_OUT,
	TOK_REDIR_OUT_APPEND,
	TOK_REDIR_ERR,
	TOK_REDIR_ERR_APPEND,
};

enum cmd_type {
	CMD_NONE,
	CMD_EXEC,
	CMD_PIPE,
	CMD_REDIR,
};

struct token {
	enum tok_type type;
	int len;
	char *lexeme;
	struct token *next;
};

struct cmd {
	enum cmd_type type;
};

struct exec_cmd {
	struct cmd base;
	char *argv[CMD_ARGS_MAX];
};

struct pipe_cmd {
	struct cmd base;
	struct cmd *left;
	struct cmd *right;
};

struct redir_cmd {
	struct cmd base;
	struct cmd *cmd;
	char *file;
	int omode;
	int fd;
};

static struct token *sh_token_new(int type, char *lexeme, int len);
static struct exec_cmd *sh_exec_cmd_new(void);
static struct pipe_cmd *sh_pipe_cmd_new(struct cmd *left, struct cmd *right);
static struct redir_cmd *sh_redir_cmd_new(struct cmd *cmd, char *file,
					  int omode, int fd);
static void sh_run_cmd(struct cmd *cmd) __attribute__((noreturn));
static void sh_end_cmd(struct cmd *cmd);
static void sh_free_tokens(struct token *toks);
static char *sh_gets(char *buf, size_t max_len);
static int sh_get_cmd(char *buf, size_t max_len);
static struct token *sh_get_token(char **input);
static struct token *sh_parse_line(char *input);
static struct cmd *sh_parse_exec(struct token *toks);
static struct cmd *sh_parse_pipe(struct token *left, struct token *right);
static struct cmd *sh_parse_redir(struct token *toks, struct token *op,
				  struct token *file);
static struct cmd *sh_parse_cmd(struct token *toks);
static void *sh_malloc(size_t size);
static pid_t sh_fork(void);
static void sh_panic(const char *str) __attribute__((noreturn));

static char cwd[CWD_BUF_SIZE];
static char buf[INPUT_BUF_SIZE];

int main(int argc, char *argv[])
{
	struct token *toks = NULL;
	struct cmd *cmd = NULL;

	printf("Simple Shell v0.1.0\n");
	printf("Type 'exit' to quit\n");

	if (!getcwd(cwd, sizeof(cwd)))
		sh_panic("getcwd failed");

	while (true) {
		printf("$ ");
		if (sh_get_cmd(buf, INPUT_BUF_SIZE) < 0)
			break;
		if (buf[0] == '\n' || buf[0] == '\r')
			continue;
		buf[strcspn(buf, "\n\r")] = 0;
		if (!strncmp(buf, "cd ", 3)) {
			if (chdir(buf + 3) < 0) {
				perror("cd failed");
				continue;
			}
			if (!getcwd(cwd, sizeof(cwd)))
				sh_panic("getcwd failed");
		} else if (!strncmp(buf, "exit", 4)) {
			printf("exit\n");
			exit(0);
		} else if (!strncmp(buf, "pwd", 3)) {
			printf("%s\n", cwd);
		} else {
			toks = sh_parse_line(buf);
			cmd = sh_parse_cmd(toks);
			if (sh_fork() == 0)
				sh_run_cmd(cmd);
			sh_end_cmd(cmd);
			cmd = NULL;
			sh_free_tokens(toks);
			toks = NULL;
			wait(NULL);
		}
	}

	printf("\nexit\n");
	return 0;
}

static struct token *sh_token_new(int type, char *lexeme, int len)
{
	struct token *tok = sh_malloc(sizeof(*tok));
	tok->type = type;
	tok->len = len;
	tok->lexeme = lexeme;
	tok->next = NULL;
	return tok;
}

static struct exec_cmd *sh_exec_cmd_new(void)
{
	struct exec_cmd *ecmd = sh_malloc(sizeof(*ecmd));
	memset(ecmd->argv, 0, sizeof(ecmd->argv));
	ecmd->base.type = CMD_EXEC;
	return ecmd;
}

static struct pipe_cmd *sh_pipe_cmd_new(struct cmd *left, struct cmd *right)
{
	struct pipe_cmd *pcmd = sh_malloc(sizeof(*pcmd));
	pcmd->left = left;
	pcmd->right = right;
	pcmd->base.type = CMD_PIPE;
	return pcmd;
}

static struct redir_cmd *sh_redir_cmd_new(struct cmd *cmd, char *file,
					  int omode, int fd)
{
	struct redir_cmd *rcmd = sh_malloc(sizeof(*rcmd));
	rcmd->cmd = cmd;
	rcmd->file = file;
	rcmd->omode = omode;
	rcmd->fd = fd;
	rcmd->base.type = CMD_REDIR;
	return rcmd;
}

static void sh_run_cmd(struct cmd *cmd)
{
	struct exec_cmd *ecmd;
	struct pipe_cmd *pcmd;
	struct redir_cmd *rcmd;
	pid_t cpid;
	int pipefd[2];
	int fd;

	switch (cmd->type) {
	case CMD_EXEC:
		ecmd = (struct exec_cmd *)cmd;
		if (!ecmd->argv[0])
			exit(1);
		execvp(ecmd->argv[0], ecmd->argv);
		dprintf(STDERR_FILENO, "exec %s failed\n", ecmd->argv[0]);
		exit(1);
		break;
	case CMD_PIPE:
		pcmd = (struct pipe_cmd *)cmd;
		if (pipe(pipefd) < 0)
			sh_panic("pipe failed");
		cpid = sh_fork();
		if (cpid == 0) {
			close(pipefd[0]);
			dup2(pipefd[1], STDOUT_FILENO);
			sh_run_cmd(pcmd->left);
		}
		cpid = sh_fork();
		if (cpid == 0) {
			close(pipefd[1]);
			dup2(pipefd[0], STDIN_FILENO);
			sh_run_cmd(pcmd->right);
		}
		close(pipefd[0]);
		close(pipefd[1]);
		wait(NULL);
		wait(NULL);
		exit(0);
		break;
	case CMD_REDIR:
		rcmd = (struct redir_cmd *)cmd;
		fd = open(rcmd->file, rcmd->omode);
		if (fd < 0) {
			dprintf(STDERR_FILENO, "cannot open %s\n", rcmd->file);
			exit(1);
		}
		dup2(fd, rcmd->fd);
		close(fd);
		sh_run_cmd(rcmd->cmd);
		break;
	default:
		exit(1);
	}
}

static void sh_end_cmd(struct cmd *cmd)
{
	if (cmd->type == CMD_PIPE) {
		sh_end_cmd(((struct pipe_cmd *)cmd)->left);
		sh_end_cmd(((struct pipe_cmd *)cmd)->right);
	} else if (cmd->type == CMD_REDIR) {
		sh_end_cmd(((struct redir_cmd *)cmd)->cmd);
	}
	free(cmd);
}

static void sh_free_tokens(struct token *toks)
{
	struct token *tok = NULL;
	while (toks) {
		tok = toks;
		toks = toks->next;
		free(tok);
	}
}

static char *sh_gets(char *buf, size_t max_len)
{
	size_t i;
	char c;

	if (!buf || max_len < 1)
		return NULL;

	for (i = 0; i + 1 < max_len;) {
		if (read(0, &c, 1) < 1)
			break;
		buf[i++] = c;
		if (c == '\n' || c == '\r')
			break;
	}
	buf[i] = 0;
	return buf;
}

static int sh_get_cmd(char *buf, size_t max_len)
{
	sh_gets(buf, max_len);
	return !buf[0] ? -1 : 0;
}

static struct token *sh_get_token(char **input)
{
	char *i = *input;
	struct token *tok = NULL;

	while (*i && strchr(WHITESPACE, *i))
		i++;

	switch (*i) {
	case 0:
		tok = sh_token_new(TOK_EOF, "", 0);
		break;
	case '|':
		tok = sh_token_new(TOK_PIPE, i, 1);
		i++;
		break;
	case '>':
		if (*(i + 1) == '>') {
			tok = sh_token_new(TOK_REDIR_OUT_APPEND, i, 2);
			i++;
		} else {
			tok = sh_token_new(TOK_REDIR_OUT, i, 1);
		}
		i++;
		break;
	case '<':
		tok = sh_token_new(TOK_REDIR_IN, i, 1);
		i++;
		break;
	case '2':
		if (*(i + 1) == '>') {
			if (*(i + 2) == '>') {
				tok = sh_token_new(TOK_REDIR_ERR_APPEND, i, 3);
				i += 3;
			} else {
				tok = sh_token_new(TOK_REDIR_ERR, i, 2);
				i += 2;
			}
			break;
		} else {
			tok = sh_token_new(TOK_ARG, i, 1);
			do {
				++i;
			} while (*i && !strchr(WHITESPACE, *i) &&
				 !strchr("2|<>", *i));
			tok->len = i - tok->lexeme;
		}
		break;
	default:
		tok = sh_token_new(TOK_ARG, i, 1);
		do {
			++i;
		} while (*i && !strchr(WHITESPACE, *i) && !strchr("2|<>", *i));
		tok->len = i - tok->lexeme;
		break;
	}
	*input = i;

	return tok;
}

static struct token *sh_parse_line(char *input)
{
	struct token *toks = NULL;
	struct token **ptoks = &toks;
	while (true) {
		*ptoks = sh_get_token(&input);
		if ((*ptoks)->type == TOK_EOF)
			break;
		ptoks = &((*ptoks)->next);
	}
	return toks;
}

static struct cmd *sh_parse_exec(struct token *toks)
{
	struct exec_cmd *ecmd = sh_exec_cmd_new();
	int argc = 0;
	struct token *tok = toks;
	while (tok->type != TOK_EOF) {
		tok->lexeme[tok->len] = 0;
		ecmd->argv[argc++] = tok->lexeme;
		tok = tok->next;
	}
	return (struct cmd *)ecmd;
}

static struct cmd *sh_parse_pipe(struct token *left, struct token *right)
{
	struct pipe_cmd *pcmd = NULL;
	if (!left || left->type != TOK_ARG)
		sh_panic("illegal command for pipe");
	if (!right || right->type == TOK_EOF)
		sh_panic("missing arguments for pipe");
	pcmd = sh_pipe_cmd_new(sh_parse_exec(left), sh_parse_cmd(right));
	return (struct cmd *)pcmd;
}

static struct cmd *sh_parse_redir(struct token *toks, struct token *op,
				  struct token *file)
{
	struct redir_cmd *rcmd = NULL;
	int omode = 0;
	int fd = 0;
	if (!file || file->type != TOK_ARG)
		sh_panic("missing arguments for redirect");
	switch (op->type) {
	case TOK_REDIR_IN:
		omode = O_RDONLY;
		fd = STDIN_FILENO;
		break;
	case TOK_REDIR_OUT:
		omode = O_WRONLY | O_CREAT | O_TRUNC;
		fd = STDOUT_FILENO;
		break;
	case TOK_REDIR_OUT_APPEND:
		omode = O_WRONLY | O_CREAT | O_APPEND;
		fd = STDOUT_FILENO;
		break;
	case TOK_REDIR_ERR:
		omode = O_WRONLY | O_CREAT | O_TRUNC;
		fd = STDERR_FILENO;
		break;
	case TOK_REDIR_ERR_APPEND:
		omode = O_WRONLY | O_CREAT | O_APPEND;
		fd = STDERR_FILENO;
		break;
	default:
		sh_panic("unsupported redirect operation");
	}
	op->type = TOK_EOF;
	file->lexeme[file->len] = 0;
	rcmd = sh_redir_cmd_new(sh_parse_exec(toks), file->lexeme, omode, fd);
	return (struct cmd *)rcmd;
}

static struct cmd *sh_parse_cmd(struct token *toks)
{
	struct token *tok = NULL;

	if (toks->type == TOK_EOF)
		return NULL;

	for (tok = toks; tok; tok = tok->next) {
		switch (tok->type) {
		case TOK_PIPE:
			tok->type = TOK_EOF;
			return sh_parse_pipe(toks, tok->next);
		case TOK_REDIR_IN:
		case TOK_REDIR_OUT:
		case TOK_REDIR_OUT_APPEND:
		case TOK_REDIR_ERR:
		case TOK_REDIR_ERR_APPEND:
			return sh_parse_redir(toks, tok, tok->next);
		case TOK_ARG:
			break;
		case TOK_EOF:
			break;
		default:
			sh_panic("illegal token");
			break;
		}
	}

	return sh_parse_exec(toks);
}

static void *sh_malloc(size_t size)
{
	void *ptr = malloc(size);
	if (!ptr)
		sh_panic("malloc failed");
	return ptr;
}

static pid_t sh_fork(void)
{
	pid_t pid = fork();
	if (pid < 0)
		sh_panic("fork failed");
	return pid;
}

static void sh_panic(const char *str)
{
	dprintf(STDERR_FILENO, "sh panic: %s\n", str);
	exit(1);
}
