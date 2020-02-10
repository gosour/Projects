#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
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
	int rsize; //rendered text size
	char *chars;
	char *render; //transformed text
} erow;

struct editorConfig {
	int cx, cy; //cursor position in "file", and maintained by us
		    //to "display" cursor wrt to terminal
	int rx;     //rx is x cursor cordinate in rendered text row
	int rowoff; //start rendering from row = rowoff
	int coloff; //start rendering from column = coloff
	int screenrows; //number of rows in current screen
	int screencols; //number of columns in current column
	int numrows;
	erow *row;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time; //status msg will time out
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

int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for(j = 0; j<cx; j++) {
		if(row->chars[j] == '\t')
			rx += (KILO_TAB_STOP -1) - (rx % KILO_TAB_STOP); //
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row) 
{
	int tabs = 0;
	int j;
	for(j=0; j<row->size; j++)
		if(row->chars[j] == '\t') tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP -1) + 1);
	
	int idx = 0;
	for(j = 0; j< row->size; j++){
		if(row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while(idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		}
		else
			row->render[idx++] = row->chars[j];	
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	
	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);
	E.numrows++;	
}

/** file io **/
void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);

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
	E.rx = E.cx;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

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

	if(E.cx < E.coloff) {
		E.coloff = E.cx;
	}
	if(E.cx >= E.coloff + E.screencols) {
		E.coloff = E.cx - E.screencols + 1;
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
			int len = E.row[filerow].rsize - E.coloff;
			if(len < 0) len = 0;
			if(len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);

	}
}

void editorDrawStatusBar(struct abuf *ab) {
	abAppend(ab, "\x1b[7m", 4); //inverts color
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines",
			E.filename ? E.filename : "[No Name]", E.numrows);
	if (len > E.screencols) len = E.screencols;
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
	abAppend(ab, status, len);	
	while (len < E.screencols) {
		if(E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {	
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3); //switches from inverted color
	abAppend(ab, "\r\n", 2);

}

void editorDrawMessageBar(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5)
	abAppend(ab, E.statusmsg, msglen);
}

void editorSetStatusMessage(const char *fmt, ...) 
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;
	editorScroll();
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H",3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	//Draw cursor
	char buf[32];
	//(E.cy - E.rowoff) prevents cursor from going out of view
	//If E.rowoff is 1 that is we have scrolled 1 line down and was because E.cy was moved 
	//1 line below view, but on offsetting with E.rowoff we will always same result if E.rowoff = 0
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void initEditor() {
	E.cx = E.cy = E.rx = 0;
	E.numrows = 0;
	E.row = NULL;
	E.rowoff = 0; 
	E.coloff = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2; //to accomodate status bar and message bar
}

/**input**/

void editorMoveCursor(int key){
	erow *row = (E.cy >= E.numrows)? NULL : &E.row[E.cy];
	switch(key) {
		case ARROW_LEFT : {
		if(E.cx != 0) 
			E.cx--; 
		else if(E.cy > 0){ //on left on column 0 go up to end of previous line
			E.cy--;
			E.cx = E.row[E.cy].size;
		}
		break;
		}
		case ARROW_RIGHT : {
			if(row && E.cx < row->size) //Allow horizontal scrolling only till end of current row
				E.cx++; 
			else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		}
		case ARROW_UP : if(E.cy != 0) E.cy--; break;
		case ARROW_DOWN : {
			if(E.cy < E.numrows)  //allows scrolling to bottom of screen but not beyond file
				E.cy++; 
			break;
		}
	}
	//This is to correct horizontal scrolling till end of current row if current row changes
	row = (E.cy >= E.numrows)? NULL : &E.row[E.cy];
	int rowlen = row? row->size : 0;
	if(E.cx > rowlen)
		E.cx = rowlen;
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
			if(E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{	//Before simulating multiple scrolls
				//set cursor to topmost or bottomost
				if( c == PAGE_UP){
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if(E.cy > E.numrows) E.cy = E.numrows;
				}
					
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

	editorSetStatusMessage("HELP: Ctrl-Q = quit");

	while(1){
		editorRefreshScreen();
		editorProcessKeyPress();
	}
	return 0;
}
