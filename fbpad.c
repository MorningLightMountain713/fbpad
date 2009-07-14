#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <linux/vt.h>
#include <fcntl.h>
#include "pad.h"
#include "term.h"
#include "util.h"

#define SHELL		"/bin/bash"
#define TAGS		8
#define CTRLKEY(x)	((x) - 96)
#define BADPOLLFLAGS	(POLLHUP | POLLERR | POLLNVAL)


static struct term terms[TAGS * 2];
static int cterm;	/* current tag */
static int lterm;	/* last tag */
static int exitit;

static int readchar(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) > 0)
		return (int) b;
	return -1;
}

static void showterm(int n)
{
	if (lterm % TAGS != cterm % TAGS)
		lterm = cterm;
	term_save(&terms[cterm]);
	cterm = n;
	term_load(&terms[cterm], TERM_REDRAW);
}

static struct term *mainterm(void)
{
	if (terms[cterm].fd)
		return &terms[cterm];
	return NULL;
}

static void directkey(void)
{
	int c = readchar();
	static int pending = 0;
	if (pending) {
		char *tags = "xnlhtrv-";
		if (strchr(tags, c))
			showterm(strchr(tags, c) - tags);
		pending = 0;
		return;
	}
	if (c == ESC) {
		switch ((c = readchar())) {
		case 'c':
			if (!mainterm())
				term_exec(SHELL);
			return;
		case 'j':
		case 'k':
			showterm((cterm + TAGS) % ARRAY_SIZE(terms));
			return;
		case 'o':
			showterm(lterm);
			return;
		case ';':
			pending = 1;
			return;
		case CTRLKEY('q'):
			exitit = 1;
			return;
		default:
			if (mainterm())
				term_send(ESC);
		}
	}
	if (c != -1)
		if (mainterm())
			term_send(c);
}

static int find_by_fd(int fd)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(terms); i++)
		if (terms[i].fd == fd)
			return i;
	return -1;
}

static int fill_ufds(struct pollfd *ufds)
{
	int n = 1;
	int i;
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	for (i = 0; i < ARRAY_SIZE(terms); i++) {
		if (terms[i].fd) {
			ufds[n].fd = terms[i].fd;
			ufds[n].events = POLLIN;
			n++;
		}
	}
	return n;
}

static void temp_switch(int termid)
{
	if (termid != cterm) {
		term_save(&terms[cterm]);
		term_load(&terms[termid], TERM_HIDDEN);
	}
}

static void switch_back(int termid)
{
	if (termid != cterm) {
		term_save(&terms[termid]);
		term_load(&terms[cterm], TERM_VISIBLE);
	}
}

static void check_ufds(struct pollfd *ufds, int n)
{
	int i;
	for (i = 1; i < n; i++) {
		int idx = find_by_fd(ufds[i].fd);
		if (ufds[i].revents & BADPOLLFLAGS) {
			temp_switch(idx);
			term_end();
			switch_back(idx);
		}
		if (ufds[i].revents & POLLIN) {
			temp_switch(idx);
			term_read();
			switch_back(idx);
		}
	}
}

static void mainloop(void)
{
	struct pollfd ufds[ARRAY_SIZE(terms) + 1];
	struct termios oldtermios, termios;
	int rv;
	int n;
	tcgetattr(STDIN_FILENO, &termios);
	oldtermios = termios;
	cfmakeraw(&termios);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios);
	memset(ufds, 0, sizeof(ufds));
	term_load(&terms[cterm], TERM_REDRAW);
	n = fill_ufds(ufds);
	while (!exitit) {
		rv = poll(ufds, n, 1000);
		if (rv == -1 && errno != EINTR)
			break;
		if (ufds[0].revents & BADPOLLFLAGS)
			break;
		if (ufds[0].revents & POLLIN)
			directkey();
		check_ufds(ufds, n);
		n = fill_ufds(ufds);
	}
	tcsetattr(STDIN_FILENO, 0, &oldtermios);
}

static void signalreceived(int n)
{
	if (exitit)
		return;
	switch (n) {
	case SIGUSR1:
		term_save(&terms[cterm]);
		ioctl(STDIN_FILENO, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		pad_shown();
		term_load(&terms[cterm], TERM_REDRAW);
		break;
	}
}

static void setupsignals(void)
{
	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	ioctl(STDIN_FILENO, VT_SETMODE, &vtm);

	signal(SIGUSR1, signalreceived);
	signal(SIGUSR2, signalreceived);
}

int main(void)
{
	char *hide = "\x1b[?25l";
	char *clear = "\x1b[2J\x1b[H";
	char *show = "\x1b[?25h";
	write(STDIN_FILENO, clear, strlen(clear));
	write(STDIN_FILENO, hide, strlen(hide));
	pad_init();
	setupsignals();
	fcntl(STDIN_FILENO, F_SETFL,
		fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	mainloop();
	pad_free();
	write(STDIN_FILENO, show, strlen(show));
	return 0;
}
