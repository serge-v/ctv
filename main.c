#include <json-c/json.h>
#include <limits.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ncursesw/ncurses.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <locale.h>
#include <termios.h>
#include "common/struct.h"
#include "common/log.h"
#include "version.h"
#include "provider.h"
#include "etvnet.h"
#include "smithsonian.h"
#include "util.h"
#include "joystick.h"

static void
synopsis()
{
	printf("usage: ctv [-hdptvc] [name]\n");
}

static void
usage()
{
	printf("ctv. Console video player for etvnet.com.\n");
	printf("Serge Voilokov, 2016.\n");
	synopsis();
	printf("options:\n"
	       "  -a    activate tv box on etvnet.com\n"
	       "  -d    dump debug output to stderr\n"
	       "  -v    print version\n"
	       );
}

static void
version()
{
	printf("ctv. Console video player for etvnet.com.\n");
	printf("Serge Voilokov, 2016.\n");
	printf("version %s\n", app_version);
	printf("date %s\n", app_date);
}

static bool dumb_term = false;
static bool activate_box = false;
static char cache_dir[PATH_MAX];
static char local_dir[PATH_MAX];
static time_t idle_start = 0;
static struct provider *provider; /* current provider */
static struct movie_list *list;   /* current list of movies from provider */
//static struct termios orig_termios;

static void
init(int argc, char **argv)
{
	int ch;

	while (optind < argc) {

		ch = getopt(argc, argv, "ahv");
		if (ch == -1)
			continue;
		switch (ch) {
			case 'a':
				activate_box = true;
				break;
			case 'h':
				usage();
				exit(1);
			case 'v':
				version();
				exit(1);
		}
	}

	snprintf(cache_dir, PATH_MAX-1, "%s/.cache/", getenv("HOME"));
	mkdir(cache_dir, 0700);
	strcat(cache_dir, "etvcc/");
	mkdir(cache_dir, 0700);

	snprintf(local_dir, PATH_MAX-1, "%s/.local/", getenv("HOME"));
	mkdir(local_dir, 0700);
	strcat(local_dir, "etvcc/");
	mkdir(local_dir, 0700);

	idle_start = time(NULL);
	const char *term = getenv("TERM");
	dumb_term = (term == NULL) || (strlen(term) == 0);
}

enum scroll_mode {
	eNames,
	eNumbers
};

struct ui
{
	int height;
	int width;
	WINDOW *win;
	enum scroll_mode scroll;
};

static void
init_ui(struct ui *ui)
{
	setlocale(LC_ALL, "");
	initscr();
	start_color();
	getmaxyx(stdscr, ui->height, ui->width);
	logi("height: %d, width: %d", ui->height, ui->width);
	init_pair(1, COLOR_RED, COLOR_BLACK);
	init_pair(2, COLOR_WHITE, COLOR_RED);
	init_pair(3, COLOR_YELLOW, COLOR_BLUE);
	refresh();
	ui->win = newwin(ui->height - 4, 76, 2, 1);
	box(ui->win, 0, 0);
	curs_set(0);
}

struct ui ui;

static void
print_list()
{
	int i;
	printf("=====================\n");
	for (i = 0; i < list->count; i++) {
		const char *sel = " ";
		struct movie_entry *e = list->items[i];

		if (i == list->sel)
			sel = "*";

		printf("%s %s", sel, e->name);

		if (i == list->sel && ui.scroll == eNumbers) {
			printf(" * ");
		} else {
			printf("   ");

		}
		printf("%d/%d\n", e->sel + 1, e->children_count);
	}
}

static void
draw_list()
{
	if (dumb_term) {
		print_list();
		return;
	}

	box(ui.win, 0, 0);

	int i;
	for (i = 0; i < list->count; i++) {

		if (i == list->sel && ui.scroll == eNames) {
			wattron(ui.win, COLOR_PAIR(1));
			mvwaddstr(ui.win, i+2, 2, "[");
		} else {
			mvwaddstr(ui.win, i+2, 2, " ");
		}

		struct movie_entry *e = list->items[i];
		mvwaddstr(ui.win, i+2, 4, e->name);

		if (i == list->sel && ui.scroll == eNames) {
			mvwaddstr(ui.win, i+2, 45, "] ");
			wattroff(ui.win, COLOR_PAIR(1));
		} else {
			mvwaddstr(ui.win, i+2, 45, "  ");
		}

//		mvwaddstr(ui.win, i+2, 39, "      ");

		if (i == list->sel && ui.scroll == eNumbers) {
			wattron(ui.win, COLOR_PAIR(1));
			mvwprintw(ui.win, i+2, 47, "%d/%d     ", e->sel + 1, e->children_count);
			mvwaddstr(ui.win, i+2, 45, "[");
			mvwaddstr(ui.win, i+2, 52, "]");
			wattroff(ui.win, COLOR_PAIR(1));
		} else {
			mvwprintw(ui.win, i+2, 47, "%d/%d     ", e->sel + 1, e->children_count);
		}

		mvwaddstr(ui.win, i+2, 54, e->on_air);
	}
}

static void
print_status(const char *msg)
{
	if (strlen(msg) > 0)
		logi(msg);
	if (dumb_term) {
		printf("status: %s\n", msg);
	} else {
		wattron(ui.win, COLOR_PAIR(3));
		mvwaddstr(ui.win, ui.height - 6, 2, "                                                                  ");
		mvwaddstr(ui.win, ui.height - 6, 3, msg);
		wattroff(ui.win, COLOR_PAIR(3));
		wrefresh(ui.win);
	}
}

enum menu_id {
	MI_ETVNET,
	MI_SMITHSONIAN,
	MI_CAMERA,
	MI_ACTIVATION,
	MI_UPDATE,
	MI_CLEAN,
	MI_REDRAW,
	MI_SHUTDOWN,
	MI_REBOOT
};


struct menu_entry {
	enum menu_id id;
	char *name;
};

struct menu_list {
	int count;
	int sel;             /* selected index */
	struct menu_entry *items;
};

static struct menu_entry menu_items[] = {
	{ MI_ETVNET, "etvnet" },
	{ MI_SMITHSONIAN, "smithsonian" },
	{ MI_CAMERA, "camera" },
	{ MI_ACTIVATION, "etvnet activate" },
	{ MI_UPDATE, "update" },
	{ MI_CLEAN, "clean" },
	{ MI_REDRAW, "redraw" },
	{ MI_SHUTDOWN, "shutdown" },
	{ MI_REBOOT, "reboot" }
};

static struct menu_list menu = {
	.count = sizeof(menu_items) / sizeof(menu_items[0]),
	.sel = 0,
	.items = menu_items
};

static bool camera_enabled = false;

static void
draw_menu()
{
	box(ui.win, 0, 0);

	int i;
	for (i = 0; i < menu.count; i++) {

		if (i == menu.sel) {
			wattron(ui.win, COLOR_PAIR(1));
			mvwaddstr(ui.win, i+2, 2, "[");
		} else {
			mvwaddstr(ui.win, i+2, 2, " ");
		}

		struct menu_entry *e = &menu.items[i];
		mvwaddstr(ui.win, i+2, 4, e->name);

		if (i == menu.sel) {
			mvwaddstr(ui.win, i+2, 20, "]");
			wattroff(ui.win, COLOR_PAIR(1));
		} else {
			mvwaddstr(ui.win, i+2, 20, " ");
		}

		mvwaddstr(ui.win, i+2, 22, "  ");

		const char *str = "";
		char buf[100];

		if (e->id == MI_CAMERA) {
			if (camera_enabled)
				str = "Enabled ";
			else
				str = "Disabled";
		} else if (e->id == MI_UPDATE) {
			snprintf(buf, 99, "%s (%s %s)", app_version, __DATE__, __TIME__);
			str = buf;
		}

		mvwaddstr(ui.win, i+2, 24, str);
	}


	struct menu_entry *e = &menu.items[menu.sel];

	if (e->id == MI_CAMERA) {
		if (camera_enabled)
			print_status("DISABLE>");
		else
			print_status("ENABLE>");
	} else {
		print_status("                      ");
	}
}

static void
print_menu()
{
	int i;
	printf("=====================\n");
	for (i = 0; i < menu.count; i++) {
		struct menu_entry *e = &menu.items[i];
		char *sel = " ";
		if (menu.sel == i)
			sel = "*";
		printf("%s  %s\n", sel, e->name);
	}
}

static void
next_movie()
{
	struct movie_entry *e = list->items[list->sel];
	if (e->children_count > 0) {
		if (++e->sel >= e->children_count)
			e->sel = 0;
	}
}

static void
prev_movie()
{
	struct movie_entry *e = list->items[list->sel];
	if (e->children_count > 0) {
		if (--e->sel < 0)
			e->sel = e->children_count - 1;
	}
}

static void
next_number()
{
	if (++list->sel >= list->count)
		list->sel = 0;
}

static void
prev_number()
{
	if (--list->sel < 0)
		list->sel = list->count - 1;
}

static void
exit_handler()
{
	if (dumb_term) {
//		tcsetattr(STDIN_FILENO,TCSAFLUSH, &orig_termios);
	} else {
		flushinp();
		endwin();
	}

	logi("stopped");
	system("tail -100 ctv.log");
}

static void
run_player(const char *url)
{
	char omxcmd[2000];
	int player_started = 0, rc, ch, quit = 0, first = 1;

	if (strcmp("/home/pi", getenv("HOME")) == 0) {
		snprintf(omxcmd, 1999,
			 "omxplayer --live "
			 "--key-config /home/pi/bin/omxp_keys.txt '%s'"
			 ">/dev/null 2>&1 &", url);
	} else {
		snprintf(omxcmd, 1999, "mplayer -msglevel all=0 -cache-min 64 '%s' 2>/dev/null 1>&2", url);
	}

	logi("starting player: %s", omxcmd);
	ch = KEY_RIGHT;

	while (!quit) {

		if (first == 0)
			ch = joystick_getch();

		first = 0;

		switch (ch) {
			case KEY_LEFT:
				if (player_started == 1) {
					print_status("stop");
					rc = system("/home/pi/src/ctv/dbuscontrol.sh stop >/dev/null 2>&1");
					logi("dbus.stop. rc: %d\r", rc);
					player_started = 0;
				}
				quit = 1;
				break;
			case KEY_RIGHT:
				if (player_started == 0) {
					print_status("start");
					rc = system(omxcmd);
					if (rc == 0)
						player_started = 1;
					logi("start omxplayer. rc: %d\r", rc);
				} else if (player_started == 1) {
					print_status("move forward 60 sec");
					rc = system("/home/pi/src/ctv/dbuscontrol.sh seek 60000000 >/dev/null 2>&1");
					logi("dbus.seek. rc: %d\r", rc);
				}
				break;
			case KEY_DOWN:
				print_status("volume down");
				rc = system("/home/pi/src/ctv/dbuscontrol.sh volumedown >/dev/null 2>&1");
				logi("dbus.volumedown. rc: %d\r", rc);
				break;
			case KEY_UP:
				print_status("volume up");
				rc = system("/home/pi/src/ctv/dbuscontrol.sh volumeup >/dev/null 2>&1");
				logi("dbus.volumeup. rc: %d\r", rc);
				break;
		}
	}

	print_status("player stopped");

}

static void
play_movie()
{
	print_status("Loading movie...");

	struct movie_entry *e = list->items[list->sel];
	if (e->children_count == 0) {
		char *url = e->stream_url;
		if (url == NULL) {
			url = provider->get_stream_url(e);
			if (provider->error_number != 0)
				statusf("no stream url: %s", provider->error());
		}
		run_player(url);
		free(url);
		return;
	}

	struct movie_entry *child = provider->get_movie(e->id, e->sel);
	if (provider->error_number != 0) {
		statusf("part %d: %s", e->sel, provider->error());
		return;
	}

	char *url = provider->get_stream_url(child);
	if (provider->error_number != 0)
		statusf("play_movie[%d]: %s", e->sel, provider->error());
	logi("id: %d, url: %s", child->id, url);
	print_status("Playing movie...");
	run_player(url);
	free(child);
	free(url);
}

static void
activate_tv_box()
{
	char *user_code;
	char *device_code;

	print_status("Connecting to etvnet.com");
	provider = etvnet_get_provider();

	provider->get_activation_code(&user_code, &device_code);
	if (provider->error_number != 0)
		err(1, "cannot get activation code: %s", provider->error());

	printf("Enter activation code on etvnet.com/Активация STB:\n    %s\n", user_code);
	printf("After entering the code hit ENTER on this box.\n");
	timeout(20);
	int rc = fgetc(stdin);
	if (rc != 10) {
		printf("Activation canceled.\n");
		exit(1);
	}

	provider->authorize(device_code);
	if (provider->error_number != 0)
		err(1, "%s", provider->error());

	printf("Activated successfully.\n");
	exit(0);
}

static void
save_selections(const char *name)
{
	int i;
	char fname[PATH_MAX];
	FILE *f;
	struct movie_entry *e;

	snprintf(fname, PATH_MAX-1, "%sselections-%s.txt", local_dir, name);
	f = fopen(fname, "wt");
	if (f == NULL)
		statusf("cannot save selections");

	fprintf(f, "list.sel = %d\n", list->sel);
	for (i = 0; i < list->count; i++) {
		e = list->items[i];
		fprintf(f, "movie.%d.sel = %d\n", e->id, e->sel);
	}

	fclose(f);
}

static void
load_selections(const char *name)
{
	int i, n, v, id, j;
	char fname[PATH_MAX];
	FILE *f;
	struct movie_entry *e;

	snprintf(fname, PATH_MAX-1, "%sselections-%s.txt", local_dir, name);
	f = fopen(fname, "rt");
	if (f == NULL)
		return;

	n = fscanf(f, "list.sel = %d\n", &v);
	if (n != 1) {
		fclose(f);
		return;
	}

	if (v >= 0 && v < list->count)
		list->sel = v;

	for (i = 0; i < list->count; i++) {
		n = fscanf(f, "movie.%d.sel = %d\n", &id, &v);
		if (n != 2)
			break;

		for (j = 0; j < list->count; j++) {
			e = list->items[j];
			if (e->id == id && v < e->children_count) {
				e->sel = v;
				break;
			}
		}
	}

	fclose(f);
	logi("selections loaded");
}

static void
turnoff_monitor()
{
	const char *cmd = "/opt/vc/bin/tvservice -o >> tvservice.log 2>&1";
	logi("turning off monitor");
	int rc = system(cmd);
	if (rc != 0)
		logwarn("error: %d. You may need to set chmod u+s /opt/vc/bin/tvservice", rc);
	else
		print_status("monitor is off");
}

static void
turnon_monitor()
{
	const char *cmd =
		"/opt/vc/bin/tvservice -p >> tvservice.log 2>&1; "
		"sleep 5; setterm --reset >> tvservice.log";
	logi("turning on monitor");
	int rc = system(cmd);
	if (rc != 0)
		logwarn("error: %d. You may need to set chmod u+s /opt/vc/bin/tvservice", rc);
	else
		print_status("monitor is on");
}

static void
on_idle()
{
	int ch = -1;

	time_t idle_interval = time(NULL) - idle_start;

	if (idle_interval < 5*60) {
		logi("on idle: %ju sec", idle_interval);
		return;
	}

	turnoff_monitor();
	while (ch == -1) {
		logi("wait for any key");
		ch = joystick_getch();
	}
	turnon_monitor();
	erase();
	refresh();
	idle_start = time(NULL);
}

static void
provider_loop(enum menu_id provider_id)
{
	int quit = 0;

	if (provider_id == MI_ETVNET) {
		print_status("Connecting to etvnet.com");
		provider = etvnet_get_provider();

	} else if (provider_id == MI_SMITHSONIAN) {
		print_status("Connecting to smithsonian.com");
		provider = smithsonian_get_provider();
	}

	if (provider->error_number != 0)
		statusf("%s", provider->error());

	print_status("Loading movie list");
	list = provider->load();

	if (provider->error_number != 0)
		statusf("%s", provider->error());

	load_selections(provider->name);

	print_status("<< MENU    SELECT_PART >>");

	while (!quit) {
		draw_list();
		save_selections(provider->name);
		wrefresh(ui.win);

		int ch = joystick_getch();

		switch (ch) {
			case -1:
				on_idle();
				break;
			case 'q': case 'Q':
				quit = 1;
				break;
			case KEY_UP:
				if (ui.scroll == eNumbers)
					prev_movie();
				else
					prev_number();
				break;
			case KEY_DOWN:
				if (ui.scroll == eNumbers)
					next_movie();
				else
					next_number();
				break;
			case KEY_LEFT:
				if (ui.scroll == eNames)
					quit = 1;
				else if (ui.scroll == eNumbers) {
					ui.scroll = eNames;
					print_status("<< MENU    SELECT_PART >>");
				}
				break;
			case KEY_RIGHT:
				if (ui.scroll == eNumbers)
					play_movie();
				else if (ui.scroll == eNames) {
					ui.scroll = eNumbers;
					print_status("<< LIST       PLAY >>");
				}
				break;
		}
	}

	werase(ui.win);
}

static void
switch_camera()
{
	int rc;
	char *cmd;

	camera_enabled = !camera_enabled;

	if (camera_enabled) {
		cmd =
			"omxplayer --live --dbus_name=camera --win 1600,0,1920,180 "
			"--key-config /home/pi/bin/omxp_keys.txt http://192.168.1.1:14:8085"
			">/dev/null 2>&1 &";
		rc = system(cmd);
		logi("camera on: %d", rc);
	} else {
		cmd = "/home/pi/src/ctv/dbuscontrol.sh stop >/dev/null 2>&1";
		rc = system(cmd);
		logi("camera off: %d", rc);
	}
}

static void
menu_action()
{
	int ch;
	enum menu_id id = menu.items[menu.sel].id;
	
	switch (id) {
	case MI_CAMERA:
		switch_camera();
		break;
	case MI_ACTIVATION:
		activate_tv_box();
		break;
	case MI_UPDATE:
		exit(3);
		break;
	case MI_ETVNET:
		werase(ui.win);
		provider_loop(MI_ETVNET);
		break;
	case MI_SMITHSONIAN:
		werase(ui.win);
		provider_loop(MI_SMITHSONIAN);
		break;
	case MI_CLEAN:
		erase();
		break;
	case MI_REDRAW:
		refresh();
		break;
	case MI_SHUTDOWN:
		print_status("SHUTDOWN>");
		ch = joystick_getch();
		if (ch == KEY_RIGHT) {
			system("sudo shutdown -h now");
			logi("shutdown: %d", errno);
			exit(1);
		}
		print_status("");
		break;		
	case MI_REBOOT:
		print_status("REBOOT>");
		ch = joystick_getch();
		if (ch == KEY_RIGHT) {
			system("sudo reboot");
			logi("reboot: %d", errno);
		}
		print_status("");
		break;
	}
}

static void
menu_loop()
{
	while (true) {
		if (dumb_term) {
			print_menu();
		} else {
			draw_menu();
			wrefresh(ui.win);
		}

		int ch = joystick_getch();

		switch (ch) {
			case 'q':
				exit(0);
			case -1:
				on_idle();
				break;
			case KEY_DOWN:
				menu.sel++;
				if (menu.sel >= menu.count)
					menu.sel = 0;
				break;
			case KEY_UP:
				menu.sel--;
				if (menu.sel < 0)
					menu.sel = menu.count - 1;
				break;
			case KEY_RIGHT:
				menu_action();
				break;
		}
	}
}

int
main(int argc, char **argv)
{
	init(argc, argv);

	if (activate_box) {
		activate_tv_box();
		return 0;
	}

	log_open("ctv.log");
	logi("=======================================");

	joystick_init();

	if (dumb_term) {
	//	tcgetattr(STDIN_FILENO, &orig_termios);
	//	struct termios raw;
	//	raw = orig_termios;
	//	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	//	raw.c_oflag &= ~(OPOST);
	//	raw.c_cflag |= (CS8);
	//	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	//	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	} else {
		init_ui(&ui);
		atexit(exit_handler);
		timeout(0);
		noecho();
		cbreak();
	}

	status_init(ui.win, ui.height - 2, dumb_term);
	menu_loop();
	
	return 0;
}
