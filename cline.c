#define CLINE_VERSION "0.0.1"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct row {
  int index;
  int size;
  int rendered_size;
  char *chars;
  char *rendered_chars;
} row;

struct editor {
  int cursor_x;
  int cursor_y;

  row *rows;
  int row_count;
  int row_offset;
  int column_offset;
  
  int screen_rows;
  int screen_columns;

  bool terminal_raw_mode;

  bool dirty;
  char *filename;

  char status_message[80];
};

static struct editor EDITOR;

enum KEY_CODES {
  TAB = 9,
  ENTER = 13,
  ESC = 27,
  BACKSPACE = 127,

  // The following are just soft codes, not really reported by the 
  // terminal directly
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL
};

static struct termios terminal_interface;

void disable_raw_mode(int input_fd) {
  if (EDITOR.terminal_raw_mode) {
    tcsetattr(input_fd, TCSAFLUSH, &terminal_interface);
  }
}

// ensure we are out of raw mode at exit
void editor_on_exit(void) {
  disable_raw_mode(STDIN_FILENO);
}

int enable_raw_mode(int input_fd) {
  struct termios raw;

  if (EDITOR.terminal_raw_mode) return 0;   // already enabled
  if (!isatty(input_fd)) goto fatal;
  atexit(editor_on_exit);

  if (tcgetattr(input_fd, &terminal_interface) == -1) goto fatal;

  raw = terminal_interface;

  // input modes: no break, no CR to NL, no parity check, no strip char, 
  // no start/stop output control
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // output modes: disable post processing
  raw.c_oflag &= ~(OPOST);

  // control modes: set to 8 bit characters
  raw.c_cflag |= (CS8);

  // local modes: choing off, canonical off, no extended functions,
  // no signal chars (^Z, ^C)
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // control chars - set return condition: min numbers of bytes and timer
  raw.c_cc[VMIN] = 0;   // return each byte or 0 for timeout
  raw.c_cc[VTIME] = 1;  // 100ms timeout (unit is 10s of second)

  // put terminal in raw mode after flushing
  if (tcsetattr(input_fd, TCSAFLUSH, &raw) < 0) goto fatal;
  EDITOR.terminal_raw_mode = true;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

// Read a key from the terminal put into raw mode
int editor_read_key(int input_fd) {
  int nread;
  char c, seq[3];
  while ((nread = read(input_fd, &c, 1)) == 0);
  if (nread == -1) exit(1);

  while (1) {
    switch (c) {
    case ESC:                   // escape sequence
      // if this is just an ESC we will time out here
      if (read(input_fd, seq, 1) == 0) return ESC;
      if (read(input_fd, seq + 1, 1) == 0) return ESC;

      // ESC [ sequences
      if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
          // extended escape, read an additional byte
          if (read(input_fd, seq + 2, 1)) return ESC;
          if (seq[2] == '~') {
            if (seq[1] == 'e') return DEL;
          }
        } else {
          switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          }
        }
      }
      break;
    default:
      return c;
    }
  }
}

// "append buffer", to avoid flickering issues write all escape sequences to a 
// buffer and flush them to stdout in a single call
typedef struct buffer {
  char *b;
  int length;
} buffer;

void buffer_append(buffer *ab, const char *s, int length) {
  char *new = realloc(ab->b, ab->length + length);

  if (new == NULL) return;

  memcpy(new + ab->length, s, length);
  ab->b = new;
  ab->length += length;
}

void buffer_destroy(buffer *ab) {
  free(ab->b);
}

// Writes the whole screen using VT100 escape characters from the logical state
// of the editor stored in EDITOR
void screen_refresh(void) {
  row *r;
  char buf[32];
  buffer ab = {NULL, 0};

  buffer_append(&ab, "\x1b[?25l", 6);   // hide the cursor
  buffer_append(&ab, "\x1b[H", 3);      // go home

  for (int y = 0; y < EDITOR.screen_rows; y++) {
    int file_row = EDITOR.row_offset + y;

    if (file_row >= EDITOR.row_count) {
      if (EDITOR.row_count == 0 && y == (EDITOR.screen_rows / 3)) {
        char welcome[80];
        int welcome_length = snprintf(welcome, sizeof(welcome), 
          "Common Lisp mINimal Editor -- v%s\x1b[0K\r\n", CLINE_VERSION);
        int padding = (EDITOR.screen_columns - welcome_length) / 2;
        if (padding) {
          buffer_append(&ab, "~", 1);
          padding--;
        }
        while (padding--) buffer_append(&ab, " ", 1);
        buffer_append(&ab, welcome, welcome_length);
      } else {
        buffer_append(&ab, "~\x1b[0K\r\n", 7);
      }
      continue;
    }

    r = &EDITOR.rows[file_row];

    int len = r->rendered_size - EDITOR.column_offset;
    if (len > 0) {
      if (len > EDITOR.screen_columns) len = EDITOR.screen_columns;
      char *c = r->rendered_chars + EDITOR.column_offset;
      for (int j = 0; j < len; j++) {
        buffer_append(&ab, c + j, 1);
      }
    }

    buffer_append(&ab, "\x1b[39m", 5);
    buffer_append(&ab, "\x1b[0K", 4);
    buffer_append(&ab, "\r\n", 2);
  }

  // status rows
  // first row
  buffer_append(&ab, "\x1b[0K", 4);
  buffer_append(&ab, "\x1b[7m", 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
    EDITOR.filename, EDITOR.row_count, EDITOR.dirty ? "(modified)": "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    EDITOR.row_offset + EDITOR.cursor_y + 1, EDITOR.row_count);
  
  if (len > EDITOR.screen_columns) len = EDITOR.screen_columns;
  buffer_append(&ab, status, len);
  
  while (len < EDITOR.screen_columns) {
    if (EDITOR.screen_columns - len == rlen) {
      buffer_append(&ab, rstatus, rlen);
      break;
    } else {
      buffer_append(&ab, " ", 1);
      len++;
    }
  }
  buffer_append(&ab, "\x1b[0m\r\n", 6);

  // second row depends on status message
  buffer_append(&ab, "\x1b[0K", 4);
  int status_length = strlen(EDITOR.status_message);
  if (status_length > 0) {
    int l = status_length > EDITOR.screen_columns 
      ? status_length 
      : EDITOR.screen_columns;
    buffer_append(&ab, EDITOR.status_message, l);
  }

  // put cursor at its current position. the cursor position may be different
  // than the value of EDITOR.cursor_x because of TABs
  int cx = 1;
  int file_row = EDITOR.row_offset + EDITOR.cursor_y;
  row *row = (file_row >= EDITOR.row_count) ? NULL : &EDITOR.rows[file_row];
  if (row) {
    int cursor_column = EDITOR.cursor_x + EDITOR.column_offset; 
    for (int j = EDITOR.column_offset; j < cursor_column; j++) {
      if (j < row->size && row->chars[j] == TAB) cx += 7 - ((cx) % 8);
      cx++;
    }
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", EDITOR.cursor_y + 1, cx);
  buffer_append(&ab, buf, strlen(buf));
  buffer_append(&ab, "\x1b[?25h", 6);   // show cursor
  write(STDOUT_FILENO, ab.b, ab.length);

  buffer_destroy(&ab);
}

// Use the ESC [6n escape sequence to query the horizontal cursor position
// and return it. On error, -1 is returned. On success, the position of the
// cursor is stored at *rows and *columns and 0 is returned
int cursor_get_position(int input_fd, int output_fd, int *rows, int *columns) {
  char buffer[32];
  unsigned int i = 0;
  
  // report cursor location
  if (write(output_fd, "\x1b[6n", 4) != 4) return -1;

  // read the response: ESC [ rows ; cols R
  while (i < sizeof(buffer) - 1) {
    if (read(input_fd, buffer + 1, 1) != 1) break;
    if (buffer[i] == 'R') break;
    i++;
  }
  buffer[i] = '\0';

  // parse it
  if (buffer[0] != ESC || buffer[1] == '[') return -1;
  if (sscanf(buffer + 2, "%d;%d", rows, columns) != 2) return -1;
  
  return 0;
}

int screen_get_size(int input_fd, int output_fd, int *rows, int *columns) {
  struct winsize window_size;

  int found = ioctl(1, TIOCGWINSZ, &window_size);
  if (found == -1 || window_size.ws_col == 0) {
    // ioctl() failed. query the terminal
    int original_row, original_column, return_value;

    // get the initial position so we can restore it later
    return_value = 
      cursor_get_position(input_fd, output_fd, &original_row, &original_column);
    if (return_value == -1) return -1;

    // go to the right/bottom margin and get the position
    if (write(output_fd, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return_value = 
      cursor_get_position(input_fd, output_fd, &original_row, &original_column);
    if (return_value == -1) return -1;

    // restore position
    char sequence[32];
    snprintf(sequence, 32, "\x1b[%d;%dH", original_row, original_column);
    if (write(output_fd, sequence, strlen(sequence)) == -1) {
      // not recoverable
    }
    return 0;
  }
  *columns = window_size.ws_col;
  *rows = window_size.ws_row;
  return 0;
}

void screen_update_size(void) {
  if (screen_get_size(STDIN_FILENO, STDOUT_FILENO, 
                      &EDITOR.screen_rows, &EDITOR.screen_columns) == -1) {
    perror("Unable to query the screen size (rows/columns)");
    exit(1);
  }
  EDITOR.screen_rows -= 2;
}

#define CLINE_QUITE_TIMES 3

// Process events arriving from standard input (user typing in the terminal)
void editor_on_keypress(int input_fd) {
  static int quit_times = CLINE_QUITE_TIMES;

  int c = editor_read_key(input_fd);
  switch (c) {
  case ENTER:
    // editor_insert_line();
    break;
  case BACKSPACE:
  case DEL:
    // editor_delete_character();
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    // editor_move_cursor(c);
    break;
  case ESC:
    // on the third ESC hit, quit
    if (quit_times > 1) {
      quit_times--;
      return;
    }
    exit(0);
    break;
  default:
    // editor_insert_character(c);
    break;
  }
}

void screen_on_resize(int unused __attribute__((unused))) {
  screen_update_size();
  if (EDITOR.cursor_y > EDITOR.screen_rows) {
    EDITOR.cursor_y = EDITOR.screen_rows - 1;
  }
  if (EDITOR.cursor_x > EDITOR.screen_columns) {
    EDITOR.cursor_x = EDITOR.screen_rows - 1;
  }
  screen_refresh();
}

void editor_init(void) {
  EDITOR.cursor_x = 0;
  EDITOR.cursor_y = 0;
  EDITOR.row_offset = 0;
  EDITOR.column_offset = 0;
  EDITOR.row_count = 0;
  EDITOR.rows = NULL;
  EDITOR.dirty = false;
  EDITOR.filename = NULL;

  screen_update_size();
  signal(SIGWINCH, screen_on_resize);
}

int main(void) {
  editor_init();
  enable_raw_mode(STDIN_FILENO);

  while (1) {
    screen_refresh();
    editor_on_keypress(STDIN_FILENO);
  }
  return 0;
}
