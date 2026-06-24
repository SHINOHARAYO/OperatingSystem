#include "lib.h"
#include "ns_proto.h"
#include "terminal_proto.h"
#include "compositor_proto.h"
#include "keyboard_proto.h"

#define FONT8x16_IMPLEMENTATION
#include "terminal_font8x16.h"

#define PAGE_SIZE 4096ULL
#define MAX_COLS 272U
#define MAX_ROWS 96U
#define SCROLLBACK_LINES 1000U
#define OUTPUT_QUEUE_BYTES 16384U
#define PRESENT_INTERVAL_MS 16U
#define OUTPUT_QUIET_MS 24U
#define FONT_CELL_WIDTH 8U
#define FONT_CELL_HEIGHT 16U

static int compositor_cap;
static int keyboard_cap;
static uint32_t *pixels;
static uint64_t pixel_bytes;
static uint32_t surface_id;
static uint32_t width;
static uint32_t height;
static uint32_t cols;
static uint32_t rows;
static uint32_t font_scale = 1;
static uint32_t cursor_col;
static uint32_t cursor_row;
static char cells[MAX_ROWS][MAX_COLS];
static uint8_t cell_colors[MAX_ROWS][MAX_COLS];
static char scrollback[SCROLLBACK_LINES][MAX_COLS];
static uint8_t scrollback_colors[SCROLLBACK_LINES][MAX_COLS];
static uint32_t scrollback_head;
static uint32_t scrollback_count;
static uint32_t view_offset;
static uint8_t current_color = 7;
static uint8_t escape_state;
static uint32_t escape_value;
static char output_queue[OUTPUT_QUEUE_BYTES];
static uint32_t output_head;
static uint32_t output_tail;
static char input_sequence[4];
static uint32_t input_sequence_count;
static uint32_t input_sequence_index;
static uint32_t pending_read_reply;
static uint64_t last_present_ms;
static uint64_t output_quiet_until_ms;
static uint8_t dirty_rows[MAX_ROWS];
static uint32_t dirty_count;
static int full_redraw;

static uint64_t round_pages(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t color) {
    if (x >= width || y >= height) return;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = pixels + (uint64_t)(y + row) * width + x;
        for (uint32_t col = 0; col < w; col++) dst[col] = color;
    }
}

static const uint8_t *glyph(char c) {
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t lower[26][7] = {
        {0,0,14,1,15,17,15}, {16,16,30,17,17,17,30},
        {0,0,14,16,16,16,14}, {1,1,15,17,17,17,15},
        {0,0,14,17,31,16,14}, {6,8,8,30,8,8,8},
        {0,0,15,17,17,15,1}, {16,16,30,17,17,17,17},
        {4,0,12,4,4,4,14}, {2,0,6,2,2,18,12},
        {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
        {0,0,26,21,21,17,17}, {0,0,30,17,17,17,17},
        {0,0,14,17,17,17,14}, {0,0,30,17,17,30,16},
        {0,0,15,17,17,15,1}, {0,0,22,24,16,16,16},
        {0,0,15,16,14,1,30}, {8,8,30,8,8,8,6},
        {0,0,17,17,17,17,15}, {0,0,17,17,17,10,4},
        {0,0,17,17,21,21,10}, {0,0,17,10,4,10,17},
        {0,0,17,17,17,15,1}, {0,0,31,2,4,8,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t dot[7] = {0,0,0,0,0,12,12};
    static const uint8_t colon[7] = {0,12,12,0,12,12,0};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t slash[7] = {1,2,2,4,8,8,16};
    static const uint8_t backslash[7] = {16,8,8,4,2,2,1};
    static const uint8_t underscore[7] = {0,0,0,0,0,0,31};
    static const uint8_t pipe[7] = {4,4,4,4,4,4,4};
    static const uint8_t greater[7] = {16,8,4,2,4,8,16};
    static const uint8_t less[7] = {1,2,4,8,4,2,1};
    static const uint8_t equals[7] = {0,0,31,0,31,0,0};
    static const uint8_t question[7] = {14,17,1,2,4,0,4};
    static const uint8_t bang[7] = {4,4,4,4,4,0,4};
    static const uint8_t comma[7] = {0,0,0,0,0,12,8};
    static const uint8_t semicolon[7] = {0,12,12,0,0,12,8};
    static const uint8_t quote[7] = {10,10,0,0,0,0,0};
    static const uint8_t apostrophe[7] = {4,4,0,0,0,0,0};
    static const uint8_t lparen[7] = {2,4,8,8,8,4,2};
    static const uint8_t rparen[7] = {8,4,2,2,2,4,8};
    static const uint8_t lbracket[7] = {14,8,8,8,8,8,14};
    static const uint8_t rbracket[7] = {14,2,2,2,2,2,14};
    static const uint8_t plus[7] = {0,4,4,31,4,4,0};
    static const uint8_t percent[7] = {17,2,4,8,16,17,0};
    static const uint8_t hash[7] = {10,31,10,10,31,10,0};
    static const uint8_t star[7] = {0,21,14,31,14,21,0};
    static const uint8_t at[7] = {14,17,23,21,23,16,14};
    static const uint8_t dollar[7] = {4,15,20,14,5,30,4};
    static const uint8_t amp[7] = {12,18,20,8,21,18,13};
    static const uint8_t caret[7] = {4,10,17,0,0,0,0};
    static const uint8_t tilde[7] = {0,0,8,21,2,0,0};
    static const uint8_t tick[7] = {8,4,0,0,0,0,0};
    static const uint8_t lbrace[7] = {2,4,4,8,4,4,2};
    static const uint8_t rbrace[7] = {8,4,4,2,4,4,8};
    if (c >= 'a' && c <= 'z') return lower[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == '.') return dot;
    if (c == ':') return colon;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '\\') return backslash;
    if (c == '_') return underscore;
    if (c == '|') return pipe;
    if (c == '>') return greater;
    if (c == '<') return less;
    if (c == '=') return equals;
    if (c == '?') return question;
    if (c == '!') return bang;
    if (c == ',') return comma;
    if (c == ';') return semicolon;
    if (c == '"') return quote;
    if (c == '\'') return apostrophe;
    if (c == '(') return lparen;
    if (c == ')') return rparen;
    if (c == '[') return lbracket;
    if (c == ']') return rbracket;
    if (c == '+') return plus;
    if (c == '%') return percent;
    if (c == '#') return hash;
    if (c == '*') return star;
    if (c == '@') return at;
    if (c == '$') return dollar;
    if (c == '&') return amp;
    if (c == '^') return caret;
    if (c == '~') return tilde;
    if (c == '`') return tick;
    if (c == '{') return lbrace;
    if (c == '}') return rbrace;
    return blank;
}

static uint32_t ansi_color(uint8_t color) {
    static const uint32_t palette[16] = {
        0x000000, 0xD75F5F, 0x63D297, 0xD7C45F,
        0x5F87D7, 0xAF6FD7, 0x5FCFD7, 0xD7E0E8,
        0x6B7280, 0xFF6B6B, 0x82E6A7, 0xF4DE78,
        0x82AFFF, 0xD69AF2, 0x7EE7EA, 0xFFFFFF
    };
    return palette[color & 15];
}

static uint32_t cell_width(void) {
    return FONT_CELL_WIDTH * font_scale;
}

static uint32_t cell_height(void) {
    return FONT_CELL_HEIGHT * font_scale;
}

static uint32_t choose_font_scale(uint32_t screen_width,
                                  uint32_t screen_height) {
    if (screen_width >= 2160 || screen_height >= 1440) return 3;
    if (screen_width >= 1800 || screen_height >= 1200) return 2;
    return 1;
}

static void draw_char_rgb(uint32_t col, uint32_t row, char c, uint32_t color) {
    const unsigned char *bits = font8x16[(uint8_t)c & 0x7F];
    uint32_t x = col * cell_width();
    uint32_t y = row * cell_height();
    for (uint32_t gy = 0; gy < FONT_CELL_HEIGHT; gy++) {
        uint8_t line = bits[gy];
        for (uint32_t gx = 0; gx < FONT_CELL_WIDTH; gx++) {
            if (line & (0x80U >> gx)) {
                fill_rect(x + gx * font_scale, y + gy * font_scale,
                          font_scale, font_scale, color);
            }
        }
    }
}

static void draw_char(uint32_t col, uint32_t row, char c, uint8_t color) {
    draw_char_rgb(col, row, c, ansi_color(color));
}

static void draw_overlay_text(uint32_t col, uint32_t row, const char *text,
                              uint32_t fg, uint32_t bg) {
    while (*text && col < cols) {
        fill_rect(col * cell_width(), row * cell_height(),
                  cell_width(), cell_height(), bg);
        draw_char_rgb(col, row, *text, fg);
        col++;
        text++;
    }
}

static void damage_rows(uint32_t top, uint32_t bottom) {
    if (surface_id == 0) return;
    uint32_t row_height = cell_height();
    uint64_t request[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DAMAGE_SURFACE,
        surface_id,
        display_pack_pair(0, top * row_height),
        display_pack_pair(width, (bottom - top + 1) * row_height)
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 32, request);
}

static void mark_row(uint32_t row) {
    if (row >= rows || full_redraw) return;
    if (!dirty_rows[row]) {
        dirty_rows[row] = 1;
        dirty_count++;
    }
}

static void mark_all(void) {
    full_redraw = 1;
    dirty_count = rows;
    for (uint32_t row = 0; row < rows; row++) dirty_rows[row] = 1;
}

static void render_dirty(void) {
    if (dirty_count == 0 || rows == 0) return;
    uint32_t row_height = cell_height();
    uint32_t total_lines = scrollback_count + rows;
    uint32_t max_offset = total_lines > rows ? total_lines - rows : 0;
    if (view_offset > max_offset) view_offset = max_offset;
    uint32_t first_line = total_lines - rows - view_offset;
    uint32_t damage_start = MAX_ROWS;
    uint32_t damage_end = 0;
    for (uint32_t row = 0; row < rows; row++) {
        if (!dirty_rows[row] && !full_redraw) continue;
        fill_rect(0, row * row_height, width, row_height, 0x000000);
        uint32_t logical = first_line + row;
        const char *line;
        const uint8_t *colors;
        if (logical < scrollback_count) {
            uint32_t index =
                (scrollback_head + logical) % SCROLLBACK_LINES;
            line = scrollback[index];
            colors = scrollback_colors[index];
        } else {
            uint32_t index = logical - scrollback_count;
            line = cells[index];
            colors = cell_colors[index];
        }
        for (uint32_t col = 0; col < cols; col++) {
            if (line[col] != ' ') {
                draw_char(col, row, line[col], colors[col]);
            }
        }
        if (view_offset == 0 && row == cursor_row && cursor_col < cols) {
            fill_rect(cursor_col * cell_width(), row * row_height,
                      cell_width(), row_height, 0x82E6A7);
            if (cells[cursor_row][cursor_col] != ' ') {
                draw_char_rgb(cursor_col, row, cells[cursor_row][cursor_col],
                              0x000000);
            }
        } else if (view_offset != 0 && row == 0 && cols >= 12) {
            draw_overlay_text(cols - 12, 0, "SCROLLBACK",
                              0x000000, 0xF4DE78);
        }
        if (damage_start == MAX_ROWS) {
            damage_start = row;
            damage_end = row;
        } else if (row == damage_end + 1) {
            damage_end = row;
        } else {
            damage_rows(damage_start, damage_end);
            damage_start = row;
            damage_end = row;
        }
        dirty_rows[row] = 0;
    }
    if (damage_start != MAX_ROWS) damage_rows(damage_start, damage_end);
    dirty_count = 0;
    full_redraw = 0;
}

static void clear_cells(void) {
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t col = 0; col < cols; col++) {
            cells[row][col] = ' ';
            cell_colors[row][col] = current_color;
        }
    }
    cursor_col = 0;
    cursor_row = 0;
    mark_all();
}

static void scroll(void) {
    uint32_t target;
    if (scrollback_count < SCROLLBACK_LINES) {
        target = (scrollback_head + scrollback_count) % SCROLLBACK_LINES;
        scrollback_count++;
    } else {
        target = scrollback_head;
        scrollback_head = (scrollback_head + 1) % SCROLLBACK_LINES;
    }
    for (uint32_t col = 0; col < cols; col++) {
        scrollback[target][col] = cells[0][col];
        scrollback_colors[target][col] = cell_colors[0][col];
    }
    for (uint32_t row = 1; row < rows; row++) {
        for (uint32_t col = 0; col < cols; col++) {
            cells[row - 1][col] = cells[row][col];
            cell_colors[row - 1][col] = cell_colors[row][col];
        }
    }
    for (uint32_t col = 0; col < cols; col++) {
        cells[rows - 1][col] = ' ';
        cell_colors[rows - 1][col] = current_color;
    }
    cursor_row = rows - 1;
    mark_all();
}

static void newline(void) {
    mark_row(cursor_row);
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= rows) scroll();
    else mark_row(cursor_row);
}

static void erase_line(void) {
    for (uint32_t col = 0; col < cols; col++) {
        cells[cursor_row][col] = ' ';
        cell_colors[cursor_row][col] = current_color;
    }
    cursor_col = 0;
    mark_row(cursor_row);
}

static void put_char(char c) {
    if (escape_state == 1) {
        escape_state = c == '[' ? 2 : 0;
        escape_value = 0;
        return;
    }
    if (escape_state == 2) {
        if (c >= '0' && c <= '9') {
            escape_value = escape_value * 10 + (uint32_t)(c - '0');
            return;
        }
        if (c == ';') return;
        if (c == 'J' && escape_value == 2) clear_cells();
        else if (c == 'K' && escape_value == 2) erase_line();
        else if (c == 'm') {
            if (escape_value == 0) current_color = 7;
            else if (escape_value >= 30 && escape_value <= 37)
                current_color = (uint8_t)(escape_value - 30);
            else if (escape_value >= 90 && escape_value <= 97)
                current_color = (uint8_t)(escape_value - 90 + 8);
        }
        else if (c == 'H') {
            mark_row(cursor_row);
            cursor_col = 0;
            cursor_row = 0;
            mark_row(cursor_row);
        } else if (c == 'C') {
            uint32_t count = escape_value ? escape_value : 1;
            mark_row(cursor_row);
            if (count > cols - 1 - cursor_col) cursor_col = cols - 1;
            else cursor_col += count;
            mark_row(cursor_row);
        } else if (c == 'D') {
            uint32_t count = escape_value ? escape_value : 1;
            mark_row(cursor_row);
            if (count > cursor_col) cursor_col = 0;
            else cursor_col -= count;
            mark_row(cursor_row);
        }
        escape_state = 0;
        return;
    }
    if (c == 27) {
        escape_state = 1;
    } else if (c == '\r') {
        mark_row(cursor_row);
        cursor_col = 0;
    } else if (c == '\n') {
        newline();
    } else if (c == '\b') {
        mark_row(cursor_row);
        if (cursor_col > 0) cursor_col--;
    } else if (c >= 32 && c < 127) {
        mark_row(cursor_row);
        cells[cursor_row][cursor_col] = c;
        cell_colors[cursor_row][cursor_col] = current_color;
        cursor_col++;
        if (cursor_col >= cols) newline();
    }
}

static void set_view_offset(uint32_t offset) {
    uint32_t max_offset = scrollback_count;
    view_offset = offset > max_offset ? max_offset : offset;
    mark_all();
    render_dirty();
    last_present_ms = sys_uptime_ms();
}

static uint32_t scroll_page_rows(void) {
    if (rows > 4) return rows - 2;
    return rows ? rows : 1;
}

static void queue_escape_sequence(char suffix) {
    input_sequence[0] = '[';
    input_sequence[1] = suffix;
    input_sequence_count = 2;
    input_sequence_index = 0;
}

static void queue_tilde_sequence(char digit) {
    input_sequence[0] = '[';
    input_sequence[1] = digit;
    input_sequence[2] = '~';
    input_sequence_count = 3;
    input_sequence_index = 0;
}

static char poll_input(void) {
    if (input_sequence_index < input_sequence_count) {
        return input_sequence[input_sequence_index++];
    }
    input_sequence_count = 0;
    input_sequence_index = 0;

    uint64_t request[IPC_INLINE_WORDS] = {KEYBOARD_REQ_READ};
    ipc_msg_t reply = sys_ipc_call((uint32_t)keyboard_cap, 0, 8, request);
    if (reply.status < 0) return 0;
    uint64_t event = reply.payload[0];
    if (KEYBOARD_EVENT_TYPE(event) != KEYBOARD_EVENT_KEY ||
        (KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_PRESS &&
         KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_REPEAT)) {
        return 0;
    }
    char c = KEYBOARD_EVENT_CHAR(event);
    if (c) {
        if (view_offset != 0) set_view_offset(0);
        return c;
    }
    uint32_t code = (uint32_t)KEYBOARD_EVENT_CODE(event);
    uint64_t mods = KEYBOARD_EVENT_MODIFIERS(event);
    if (code == 103) {
        queue_escape_sequence('A');
        return 27;
    } else if (code == 108) {
        queue_escape_sequence('B');
        return 27;
    } else if (code == 106) {
        queue_escape_sequence('C');
        return 27;
    } else if (code == 105) {
        queue_escape_sequence('D');
        return 27;
    } else if (code == 102 && (mods & KEYBOARD_MOD_CTRL)) {
        set_view_offset(scrollback_count);
    } else if (code == 107 && (mods & KEYBOARD_MOD_CTRL)) {
        set_view_offset(0);
    } else if (code == 102) {
        queue_escape_sequence('H');
        return 27;
    } else if (code == 107) {
        queue_escape_sequence('F');
        return 27;
    } else if (code == 104) set_view_offset(view_offset + scroll_page_rows());
    else if (code == 109)
        set_view_offset(view_offset > scroll_page_rows() ?
                        view_offset - scroll_page_rows() : 0);
    else if (code == 111) {
        queue_tilde_sequence('3');
        return 27;
    }
    return 0;
}

static uint32_t output_next(uint32_t index) {
    return (index + 1) % OUTPUT_QUEUE_BYTES;
}

static void drain_output(void) {
    while (output_head != output_tail) {
        put_char(output_queue[output_head]);
        output_head = output_next(output_head);
    }
}

static void queue_output(const char *bytes, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint32_t next = output_next(output_tail);
        if (next == output_head) drain_output();
        output_queue[output_tail] = bytes[i];
        output_tail = next;
    }
}

static void present_if_due(int force) {
    drain_output();
    uint64_t now = sys_uptime_ms();
    if (force || now - last_present_ms >= PRESENT_INTERVAL_MS) {
        render_dirty();
        last_present_ms = now;
    }
}

static int initialize_surface(void) {
    uint64_t info_request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_INFO};
    ipc_msg_t info =
        sys_ipc_call((uint32_t)compositor_cap, 0, 8, info_request);
    if (info.status < 0) return -1;
    uint32_t screen_width = (uint32_t)(info.payload[0] >> 32);
    uint32_t screen_height = (uint32_t)info.payload[0];
    font_scale = choose_font_scale(screen_width, screen_height);
    cols = (screen_width - 32) / cell_width();
    rows = (screen_height - 32) / cell_height();
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    width = cols * cell_width();
    height = rows * cell_height();
    pixel_bytes = round_pages((uint64_t)width * height * sizeof(uint32_t));
    pixels = (uint32_t *)sys_mmap(pixel_bytes);
    if (!pixels) return -1;
    clear_cells();
    fill_rect(0, 0, width, height, 0x000000);

    int mem_cap = sys_mem_export(pixels, pixel_bytes,
                                  MEM_RIGHT_READ | MEM_RIGHT_WRITE |
                                  MEM_RIGHT_LEND);
    if (mem_cap < 0) return -1;
    uint64_t create[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)mem_cap,
        [1] = 0,
        [2] = pixel_bytes,
        [3] = MEM_RIGHT_READ | IPC_MEM_MODE_LEND,
        [4] = COMPOSITOR_REQ_CREATE_SURFACE,
        [5] = display_pack_pair(width, height),
        [6] = display_pack_pair((screen_width - width) / 2,
                                (screen_height - height) / 2),
        [7] = width
    };
    ipc_msg_t created =
        sys_ipc_call((uint32_t)compositor_cap, IPC_FLAG_MEM, 64, create);
    if (created.status < 0 || created.payload[0] == 0) return -1;
    surface_id = (uint32_t)created.payload[0];
    uint64_t flags[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_SET_SURFACE_FLAGS,
        surface_id,
        COMPOSITOR_SURFACE_FLAG_FOCUSABLE | COMPOSITOR_SURFACE_FLAG_CHROME
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 24, flags);
    mark_all();
    render_dirty();
    return 0;
}

void _start(void) {
    while ((compositor_cap = ns_resolve("compositor")) < 0) sys_sleep(10);
    while ((keyboard_cap = ns_resolve("keyboard")) < 0) sys_sleep(10);
    if (initialize_surface() < 0) sys_exit(1);
    while (ns_register("terminal") < 0) sys_sleep(10);

    last_present_ms = sys_uptime_ms();
    while (1) {
        if (sys_uptime_ms() >= output_quiet_until_ms) {
            present_if_due(0);
        }
        if (pending_read_reply != 0) {
            char c = poll_input();
            if (c) {
                uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
                    (uint64_t)(uint8_t)c
                };
                uint32_t reply_cap = pending_read_reply;
                pending_read_reply = 0;
                sys_ipc_reply(reply_cap, 0, 0, 8, reply);
            }
        }

        ipc_msg_t msg = sys_ipc_recv_timeout(CAP_SELF, 8);
        if ((int)msg.reply_cap < 0) {
            continue;
        }
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
        int status = 0;
        int should_reply = 1;
        if (msg.payload[0] == TERMINAL_REQ_WRITE) {
            const char *bytes = (const char *)&msg.payload[1];
            uint64_t count = msg.len > 8 ? msg.len - 8 : 0;
            if (count > IPC_INLINE_BYTES - 8) count = IPC_INLINE_BYTES - 8;
            queue_output(bytes, count);
            output_quiet_until_ms = sys_uptime_ms() + OUTPUT_QUIET_MS;
        } else if (msg.payload[0] == TERMINAL_REQ_READ) {
            if (pending_read_reply != 0) {
                status = -1;
            } else {
                pending_read_reply = msg.reply_cap;
                should_reply = 0;
            }
        } else if (msg.payload[0] == TERMINAL_REQ_FLUSH) {
            output_quiet_until_ms = 0;
            present_if_due(1);
        } else {
            status = -1;
        }
        if (should_reply) {
            sys_ipc_reply(msg.reply_cap, status, 0, 0, reply);
        }
    }
}
