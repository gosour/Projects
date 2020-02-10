#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f) 

enum editorKey {
	ARROW_LEFT = 1000, //initialized so that does not conflict with char values being read
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

typedef struct erow {
	int size;
	char *chars;
} erow;

struct editorConfig {
	int cx, cy; //cursor position in "file", and maintained by us
		    //to "display" cursor wrt to terminal
	int rowoff; //what row of file we are currently scrolled to (ie displayed from)
	int screenrows; //number of rows in current screen
	int screencols; //number of columns in current column
	int numrows;
	erow *row;
	struct termios orig_termios;
} E;

/**terminal**/
void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(s);
	exit(1);
}

void disableRawMode() {
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("library");
}

void enableRawMode() {
	struct termios raw;

	if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("library");
	atexit(disableRawMode);

	tcgetattr(STDIN_FILENO, &raw);
 	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN) ;
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("library");
}

int editorReadKey() { //returns int insread of char to account for all the enums
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1) die("read");
  }
  if(c == '\x1b') { //if escape read 2 more bytes;
	char seq[3];
	if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b'; //if read times out return just escape
	if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b'; 

	if(seq[0] == '[') {
		if(seq[1] >= '0' && seq[1] <= '9') {
		 if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
			if (seq[2] == '~') {
			  switch (seq[1]) {
			    case '1': return HOME_KEY;
			    case '3': return DEL_KEY;
			    case '4': return END_KEY;
			    case '5': return PAGE_UP;
			    case '6': return PAGE_DOWN;
			    case '7': return HOME_KEY;
			    case '8': return END_KEY;
			  }
			}
		} else {
			switch (seq[1]) {
				case 'A' : return ARROW_UP;
				case 'B' : return ARROW_DOWN;
				case 'C' : return ARROW_RIGHT;
				case 'D' : return ARROW_LEFT;
				case 'H' : return HOME_KEY;
				case 'F' : return END_KEY;
			}
		 }
	} else if (seq[0] == 'O') {
      		switch (seq[1]) {
        	case 'H': return HOME_KEY;
        	case 'F': return END_KEY;
      		}
    	}
	return '\x1b';
  }
  return c;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
		return -1;
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}
/** row operations **/
void editorAppendRow(char *s, size_t len) {
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	
	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.numrows++;	
}

/** file io **/
void editorOpen(char *filename) {
	FILE *fp = fopen(filename,"r");
	if(!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0; //0 meaning getline will allocate
	ssize_t linelen;
	while( (linelen = getline(&line, &linecap, fp)) != -1) {
		while(linelen > 0 && (line[linelen -1] == '\r' ||
					line[linelen-1] == '\n')) //Strip terminal chars
			linelen--;
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
}

/**append buffer**/
struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char *s, int len){
	char *new = realloc(ab->b, ab->len+len);
	if(!new) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}


/**output**/

void editorScroll() {
	if(E.cy < E.rowoff) { //checks if cursor is above viewing window
	//if E.rowoff == n and E.cy is n-1
	//this means we want to display from E.rowoff n-1
		E.rowoff = E.cy; 
	}
	if(E.cy >= E.rowoff + E.screenrows) { //checks for cursor moves past viewing window bottom
	//if cursor scrolls past -> rowoff(top of file) plus height of screen 
	//for each line of cursor moving bottom rowoff increases by 1
	//this means the file will be displayed from the next line at top from E.rowoff
		E.rowoff = E.cy - E.screenrows + 1;
	}
}

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows ; y++) {
		int filerow = y + E.rowoff;
		if(filerow >= E.numrows) {
			if(E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
				 "Kilo Editor -- version %s", KILO_VERSION);
				 if(welcomelen > E.screencols) welcomelen = E.screencols;
				 int padding = (E.screencols - welcomelen) / 2;
				 if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				 }
				 while(padding--) abAppend(ab, " ", 1);
				 abAppend(ab, welcome, welcomelen);
			} else {
			abAppend(ab, "~", 1);
			}
		}
		else {
			int len = E.row[filerow].size;
			if(len > E.screencols) len = E.screencols;
			abAppend(ab, E.row[filerow].chars, len);
		}
		abAppend(ab, "\x1b[K", 3);
		if(y < E.screenrows - 1)
			abAppend(ab, "\r\n", 2);

	}
}

void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;
	editorScroll();
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H",3);

	editorDrawRows(&ab);
	char buf[32];
	//(E.cy - E.rowoff) prevents cursor from going out of view
	//If E.rowoff is 1 that is we have scrolled 1 line down and was because E.cy was moved 
	//1 line below view, but on offsetting with E.rowoff we will always same result if E.rowoff = 0
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void initEditor() {
	E.cx = E.cy = 0;
	E.numrows = 0;
	E.row = NULL;
	E.rowoff = 0; 
	if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

/**input**/

void editorMoveCursor(int key){
	switch(key) {
		case ARROW_LEFT : if(E.cx != 0) E.cx--; break;
		case ARROW_RIGHT : if (E.cx != E.screencols - 1) E.cx++; break;
		case ARROW_UP : if(E.cy != 0) E.cy--; break;
		case ARROW_DOWN : {
			if(E.cy < E.numrows)  //allows scrolling to bottom of screen but not beyond file
				E.cy++; 
			break;
		}
	}
}

void editorProcessKeyPress() {
	int c = editorReadKey();
	switch (c) {
		case CTRL_KEY('q') :
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case HOME_KEY: 
			E.cx = 0;
			break;
		case END_KEY:
			E.cx = E.screencols - 1;
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while(times--)
					editorMoveCursor(c == PAGE_UP? ARROW_UP: ARROW_DOWN);
			
			}
			break;
		case ARROW_UP :
		case ARROW_DOWN :
		case ARROW_LEFT :
		case ARROW_RIGHT :
			editorMoveCursor(c);
			break;
	}
}
int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if(argc >= 2)
		editorOpen(argv[1]);

	while(1){
		editorRefreshScreen();
		editorProcessKeyPress();
	}
	return 0;
}
