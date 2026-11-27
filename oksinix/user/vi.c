/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 modal full-screen text editor (/mnt/VI.ELF)
 * Uses Linux i386 int $0x80 syscalls + ANSI/VT100 terminal escape sequences.
 */

typedef unsigned char uint8_t;
typedef unsigned int  uint32_t;
typedef int           int32_t;
typedef uint32_t      size_t;
typedef int           bool_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

extern int32_t __oksinix_int80(uint32_t nr, uint32_t a1, uint32_t a2,
                               uint32_t a3, uint32_t a4, uint32_t a5);

#define SYS_read       3U
#define SYS_write      4U
#define SYS_open       5U
#define SYS_close      6U
#define SYS_nanosleep  162U

#define O_RDONLY       0x0000U
#define O_WRONLY       0x0001U
#define O_CREAT        0x0040U
#define O_TRUNC        0x0200U

#define CON_COLS         80
#define CON_ROWS         25
#define VI_MAX_PATH      128
#define VI_MAX_FILE_SZ   8192

#define VI_MAX_LINES     64
#define VI_LINE_MAX      (CON_COLS - 1)
#define VI_VIEW_ROWS     (CON_ROWS - 1)

#define VI_MODE_NORMAL   0
#define VI_MODE_INSERT   1
#define VI_MODE_COMMAND  2
#define VI_MODE_VISUAL   3
#define VI_MODE_VLINE    4

#define VI_YANK_CHAR_MAX (VI_MAX_LINES * (VI_LINE_MAX + 1))

struct vi_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct vi_mouse_state {
    int32_t  x;
    int32_t  y;
    int32_t  max_x;
    int32_t  max_y;
    uint8_t  buttons;
    uint32_t packets;
};

int main(int argc, char **argv);

static char    vi_lines[VI_MAX_LINES][VI_LINE_MAX + 1];
static int32_t vi_num_lines = 1;
static int32_t vi_cur_row   = 0;
static int32_t vi_cur_col   = 0;
static int32_t vi_top_row   = 0;
static int     vi_mode      = VI_MODE_NORMAL;
static bool_t  vi_dirty     = FALSE;
static bool_t  vi_pending_d = FALSE;
static bool_t  vi_pending_y = FALSE;

static int32_t vi_vis_row   = 0;
static int32_t vi_vis_col   = 0;

static bool_t  vi_yank_linewise = FALSE;
static char    vi_yank_lines[VI_MAX_LINES][VI_LINE_MAX + 1];
static int32_t vi_yank_num_lines = 0;
static char    vi_yank_chars[VI_YANK_CHAR_MAX];
static int32_t vi_yank_char_len = 0;

static char    vi_filepath[VI_MAX_PATH];
static char    vi_cmd_buf[64];
static int32_t vi_cmd_len   = 0;
static char    vi_status_msg[CON_COLS + 1];

static char    vi_save_buf[VI_MAX_FILE_SZ];
static char    vi_frame_buf[4096];
static int     vi_serial_fd = -1;

static int sys_open(const char *path, uint32_t flags, uint32_t mode)
{
    return (int)__oksinix_int80(SYS_open, (uint32_t)path, flags, mode, 0U, 0U);
}

static int32_t sys_read(int fd, void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_read, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int32_t sys_write(int fd, const void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_write, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int sys_close(int fd)
{
    return (int)__oksinix_int80(SYS_close, (uint32_t)fd, 0U, 0U, 0U, 0U);
}

static void sys_sleep_ms(int32_t ms)
{
    struct vi_timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = ms * 1000000;
    (void)__oksinix_int80(SYS_nanosleep, (uint32_t)&ts, 0U, 0U, 0U, 0U);
}

static size_t vi_strlen(const char *s)
{
    size_t n = 0U;
    if (s == NULL) {
        return 0U;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int vi_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static int vi_strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0U && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0U) {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static void vi_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

static void vi_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0U) {
        *d++ = (uint8_t)c;
    }
}

static char *vi_fmt_uint(char *dst, char *end, uint32_t val)
{
    char tmp[16];
    int i = 0;
    if (dst == NULL || end == NULL || dst > end) {
        return dst;
    }
    if (val == 0U) {
        if (dst < end) {
            *dst++ = '0';
        }
    } else {
        while (val > 0U) {
            tmp[i++] = (char)('0' + (val % 10U));
            val /= 10U;
        }
        while (i > 0 && dst < end) {
            *dst++ = tmp[--i];
        }
    }
    *dst = '\0';
    return dst;
}

static char *vi_fmt_str(char *dst, char *end, const char *src)
{
    if (dst == NULL || end == NULL || dst > end) {
        return dst;
    }
    if (src != NULL) {
        while (*src != '\0' && dst < end) {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return dst;
}

static void vi_serial_log(const char *action, const char *path, uint32_t lines, uint32_t bytes)
{
    char msg[256];
    char *p = msg;
    char *end = msg + sizeof(msg) - 1U;

    if (vi_serial_fd < 0) {
        return;
    }
    p = vi_fmt_str(p, end, "[VI] ");
    p = vi_fmt_str(p, end, action);
    p = vi_fmt_str(p, end, " \"");
    p = vi_fmt_str(p, end, path);
    p = vi_fmt_str(p, end, "\" (");
    p = vi_fmt_uint(p, end, lines);
    p = vi_fmt_str(p, end, " lines, ");
    p = vi_fmt_uint(p, end, bytes);
    p = vi_fmt_str(p, end, " bytes)\n");
    (void)sys_write(vi_serial_fd, msg, (uint32_t)(p - msg));
}

static void vi_clamp_cursor(void)
{
    int32_t len;

    if (vi_cur_row < 0) {
        vi_cur_row = 0;
    }
    if (vi_cur_row >= vi_num_lines) {
        vi_cur_row = vi_num_lines - 1;
    }

    len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
    if (vi_mode == VI_MODE_INSERT) {
        if (vi_cur_col > len) {
            vi_cur_col = len;
        }
    } else {
        if (len == 0) {
            vi_cur_col = 0;
        } else if (vi_cur_col >= len) {
            vi_cur_col = len - 1;
        }
    }
    if (vi_cur_col < 0) {
        vi_cur_col = 0;
    }

    if (vi_cur_row < vi_top_row) {
        vi_top_row = vi_cur_row;
    }
    if (vi_cur_row >= vi_top_row + VI_VIEW_ROWS) {
        vi_top_row = vi_cur_row - VI_VIEW_ROWS + 1;
    }
}

static void vi_load_file(const char *path)
{
    int fd;
    uint8_t buf[64];
    int32_t n;
    uint32_t total_bytes = 0U;

    vi_memset(vi_lines, 0, sizeof(vi_lines));
    vi_num_lines  = 1;
    vi_cur_row    = 0;
    vi_cur_col    = 0;
    vi_top_row    = 0;
    vi_dirty      = FALSE;
    vi_pending_d  = FALSE;
    vi_pending_y  = FALSE;

    fd = sys_open(path, O_RDONLY, 0U);
    if (fd < 0) {
        char *p = vi_status_msg;
        char *end = vi_status_msg + sizeof(vi_status_msg) - 1U;
        p = vi_fmt_str(p, end, "\"");
        p = vi_fmt_str(p, end, path);
        p = vi_fmt_str(p, end, "\" [New File]");
        return;
    }

    {
        int32_t row = 0;
        int32_t col = 0;

        while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
            int32_t i;
            for (i = 0; i < n; i++) {
                char c = (char)buf[i];
                total_bytes++;
                if (c == '\r') {
                    continue;
                }
                if (c == '\n') {
                    vi_lines[row][col] = '\0';
                    if (row + 1 < VI_MAX_LINES) {
                        row++;
                        col = 0;
                    }
                } else if (c >= 32 && c <= 126) {
                    if (col < VI_LINE_MAX) {
                        vi_lines[row][col++] = c;
                        vi_lines[row][col] = '\0';
                    }
                }
            }
        }
        (void)sys_close(fd);

        if (row > 0 && col == 0) {
            vi_num_lines = row;
        } else {
            vi_num_lines = row + 1;
        }
    }

    {
        char *p = vi_status_msg;
        char *end = vi_status_msg + sizeof(vi_status_msg) - 1U;
        p = vi_fmt_str(p, end, "\"");
        p = vi_fmt_str(p, end, path);
        p = vi_fmt_str(p, end, "\" ");
        p = vi_fmt_uint(p, end, (uint32_t)vi_num_lines);
        p = vi_fmt_str(p, end, "L, ");
        p = vi_fmt_uint(p, end, total_bytes);
        p = vi_fmt_str(p, end, "B");
    }

    vi_serial_log("loaded", path, (uint32_t)vi_num_lines, total_bytes);
}

static int vi_save_file(const char *path)
{
    uint32_t pos = 0U;
    int32_t r;
    int fd;

    if (path == NULL || path[0] == '\0') {
        (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                         "E32: No file name");
        return -1;
    }

    for (r = 0; r < vi_num_lines; r++) {
        size_t len = vi_strlen(vi_lines[r]);
        if (pos + (uint32_t)len + 1U < sizeof(vi_save_buf)) {
            vi_memcpy(&vi_save_buf[pos], vi_lines[r], len);
            pos += (uint32_t)len;
            vi_save_buf[pos++] = '\n';
        }
    }

    fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644U);
    if (fd < 0) {
        (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                         "E212: Cannot open file for writing");
        return -1;
    }

    if (pos > 0U) {
        (void)sys_write(fd, vi_save_buf, pos);
    }
    (void)sys_close(fd);

    vi_dirty = FALSE;
    {
        char *p = vi_status_msg;
        char *end = vi_status_msg + sizeof(vi_status_msg) - 1U;
        p = vi_fmt_str(p, end, "\"");
        p = vi_fmt_str(p, end, path);
        p = vi_fmt_str(p, end, "\" ");
        p = vi_fmt_uint(p, end, (uint32_t)vi_num_lines);
        p = vi_fmt_str(p, end, "L, ");
        p = vi_fmt_uint(p, end, pos);
        p = vi_fmt_str(p, end, "B written");
    }

    vi_serial_log("saved", path, (uint32_t)vi_num_lines, pos);
    return 0;
}

static void vi_insert_line_at(int32_t idx)
{
    int32_t r;
    if (vi_num_lines >= VI_MAX_LINES) {
        return;
    }
    for (r = vi_num_lines; r > idx; r--) {
        vi_memcpy(vi_lines[r], vi_lines[r - 1], VI_LINE_MAX + 1);
    }
    vi_memset(vi_lines[idx], 0, VI_LINE_MAX + 1);
    vi_num_lines++;
    vi_dirty = TRUE;
}

static void vi_delete_line_at(int32_t idx)
{
    int32_t r;
    if (vi_num_lines <= 1) {
        vi_lines[0][0] = '\0';
        vi_cur_col = 0;
        vi_dirty = TRUE;
        return;
    }
    for (r = idx; r < vi_num_lines - 1; r++) {
        vi_memcpy(vi_lines[r], vi_lines[r + 1], VI_LINE_MAX + 1);
    }
    vi_memset(vi_lines[vi_num_lines - 1], 0, VI_LINE_MAX + 1);
    vi_num_lines--;
    if (vi_cur_row >= vi_num_lines) {
        vi_cur_row = vi_num_lines - 1;
    }
    vi_dirty = TRUE;
}

static void vi_insert_char(char c)
{
    char *line = vi_lines[vi_cur_row];
    int32_t len = (int32_t)vi_strlen(line);
    int32_t i;

    if (len >= VI_LINE_MAX) {
        return;
    }
    for (i = len; i >= vi_cur_col; i--) {
        line[i + 1] = line[i];
    }
    line[vi_cur_col] = c;
    vi_cur_col++;
    vi_dirty = TRUE;
}

static void vi_delete_char_at(int32_t row, int32_t col)
{
    char *line = vi_lines[row];
    int32_t len = (int32_t)vi_strlen(line);
    int32_t i;

    if (col < 0 || col >= len) {
        return;
    }
    for (i = col; i < len; i++) {
        line[i] = line[i + 1];
    }
    vi_dirty = TRUE;
}

static void vi_split_line_at_cursor(void)
{
    char tail[VI_LINE_MAX + 1];
    char *line = vi_lines[vi_cur_row];
    size_t tail_len = vi_strlen(&line[vi_cur_col]);

    if (vi_num_lines >= VI_MAX_LINES) {
        return;
    }
    vi_memcpy(tail, &line[vi_cur_col], tail_len + 1U);
    line[vi_cur_col] = '\0';

    vi_insert_line_at(vi_cur_row + 1);
    vi_memcpy(vi_lines[vi_cur_row + 1], tail, tail_len + 1U);
    vi_cur_row++;
    vi_cur_col = 0;
    vi_dirty = TRUE;
}

static void vi_backspace_in_insert(void)
{
    if (vi_cur_col > 0) {
        vi_cur_col--;
        vi_delete_char_at(vi_cur_row, vi_cur_col);
    } else if (vi_cur_row > 0) {
        int32_t prev_len = (int32_t)vi_strlen(vi_lines[vi_cur_row - 1]);
        int32_t cur_len  = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
        int32_t copy_len = cur_len;

        if (prev_len + copy_len > VI_LINE_MAX) {
            copy_len = VI_LINE_MAX - prev_len;
        }
        if (copy_len > 0) {
            vi_memcpy(&vi_lines[vi_cur_row - 1][prev_len], vi_lines[vi_cur_row], (size_t)copy_len);
            vi_lines[vi_cur_row - 1][prev_len + copy_len] = '\0';
        }
        vi_delete_line_at(vi_cur_row);
        vi_cur_row--;
        vi_cur_col = prev_len;
    }
}

static void vi_get_visual_range(int32_t *sr, int32_t *sc, int32_t *er, int32_t *ec)
{
    int32_t r1 = vi_vis_row;
    int32_t c1 = vi_vis_col;
    int32_t r2 = vi_cur_row;
    int32_t c2 = vi_cur_col;

    if (r1 < r2 || (r1 == r2 && c1 <= c2)) {
        *sr = r1;
        *sc = c1;
        *er = r2;
        *ec = c2;
    } else {
        *sr = r2;
        *sc = c2;
        *er = r1;
        *ec = c1;
    }

    if (vi_mode == VI_MODE_VLINE) {
        int32_t end_len = (int32_t)vi_strlen(vi_lines[*er]);
        *sc = 0;
        *ec = (end_len > 0) ? (end_len - 1) : 0;
    }
}

static bool_t vi_is_cell_selected(int32_t row, int32_t col,
                                  int32_t sr, int32_t sc, int32_t er, int32_t ec)
{
    if (row < sr || row > er) {
        return FALSE;
    }
    if (vi_mode == VI_MODE_VLINE) {
        return TRUE;
    }
    if (sr == er) {
        return (col >= sc && col <= ec) ? TRUE : FALSE;
    }
    if (row == sr) {
        return (col >= sc) ? TRUE : FALSE;
    }
    if (row == er) {
        return (col <= ec) ? TRUE : FALSE;
    }
    return TRUE;
}

static void vi_yank_line_range(int32_t start_row, int32_t end_row)
{
    int32_t r;
    int32_t cnt = 0;
    char *p;

    if (start_row < 0) {
        start_row = 0;
    }
    if (end_row >= vi_num_lines) {
        end_row = vi_num_lines - 1;
    }
    if (start_row > end_row) {
        return;
    }

    for (r = start_row; r <= end_row && cnt < VI_MAX_LINES; r++) {
        vi_memcpy(vi_yank_lines[cnt], vi_lines[r], VI_LINE_MAX + 1);
        cnt++;
    }
    vi_yank_num_lines = cnt;
    vi_yank_linewise  = TRUE;
    vi_yank_char_len  = 0;

    p = vi_status_msg;
    p = vi_fmt_uint(p, vi_status_msg + sizeof(vi_status_msg) - 1U, (uint32_t)cnt);
    p = vi_fmt_str(p, vi_status_msg + sizeof(vi_status_msg) - 1U,
                   (cnt == 1) ? " line yanked" : " lines yanked");
}

static void vi_yank_char_range(int32_t sr, int32_t sc, int32_t er, int32_t ec)
{
    int32_t r;
    int32_t out_len = 0;
    char *p;

    if (sr < 0) {
        sr = 0;
    }
    if (er >= vi_num_lines) {
        er = vi_num_lines - 1;
    }
    if (sr > er) {
        return;
    }

    for (r = sr; r <= er; r++) {
        int32_t len = (int32_t)vi_strlen(vi_lines[r]);
        int32_t c_start = (r == sr) ? sc : 0;
        int32_t c_end   = (r == er) ? ec : (len - 1);
        int32_t c;

        if (c_start < 0) {
            c_start = 0;
        }
        if (c_end >= len) {
            c_end = len - 1;
        }
        for (c = c_start; c <= c_end && out_len < VI_YANK_CHAR_MAX; c++) {
            vi_yank_chars[out_len++] = vi_lines[r][c];
        }
        if (r < er && out_len < VI_YANK_CHAR_MAX) {
            vi_yank_chars[out_len++] = '\n';
        }
    }

    vi_yank_char_len  = out_len;
    vi_yank_linewise  = FALSE;
    vi_yank_num_lines = 0;

    p = vi_status_msg;
    p = vi_fmt_uint(p, vi_status_msg + sizeof(vi_status_msg) - 1U, (uint32_t)out_len);
    p = vi_fmt_str(p, vi_status_msg + sizeof(vi_status_msg) - 1U,
                   (out_len == 1) ? " char yanked" : " chars yanked");
}

static void vi_delete_visual_selection(void)
{
    int32_t sr, sc, er, ec;

    vi_get_visual_range(&sr, &sc, &er, &ec);

    if (vi_mode == VI_MODE_VLINE) {
        int32_t count = er - sr + 1;
        int32_t i;
        vi_yank_line_range(sr, er);
        for (i = 0; i < count; i++) {
            vi_delete_line_at(sr);
        }
        vi_cur_row = sr;
        vi_cur_col = 0;
        return;
    }

    vi_yank_char_range(sr, sc, er, ec);

    if (sr == er) {
        char *line = vi_lines[sr];
        int32_t len = (int32_t)vi_strlen(line);
        if (len > 0 && sc < len) {
            int32_t del_end = (ec < len) ? ec : (len - 1);
            int32_t tail_start = del_end + 1;
            int32_t tail_len = len - tail_start;
            if (tail_len > 0) {
                vi_memcpy(&line[sc], &line[tail_start], (size_t)tail_len + 1U);
            } else {
                line[sc] = '\0';
            }
            vi_dirty = TRUE;
        }
    } else {
        int32_t end_len = (int32_t)vi_strlen(vi_lines[er]);
        int32_t tail_start = ec + 1;
        int32_t tail_len = (tail_start < end_len) ? (end_len - tail_start) : 0;
        int32_t copy_len = tail_len;
        int32_t count = er - sr;
        int32_t i;

        if (sc > (int32_t)vi_strlen(vi_lines[sr])) {
            sc = (int32_t)vi_strlen(vi_lines[sr]);
        }
        if (sc + copy_len > VI_LINE_MAX) {
            copy_len = VI_LINE_MAX - sc;
        }
        if (copy_len > 0) {
            vi_memcpy(&vi_lines[sr][sc], &vi_lines[er][tail_start], (size_t)copy_len);
        }
        vi_lines[sr][sc + copy_len] = '\0';

        for (i = 0; i < count; i++) {
            vi_delete_line_at(sr + 1);
        }
        vi_dirty = TRUE;
    }

    vi_cur_row = sr;
    vi_cur_col = sc;
}

static void vi_paste(bool_t before)
{
    if (vi_yank_linewise) {
        int32_t insert_at;
        int32_t i;

        if (vi_yank_num_lines <= 0) {
            return;
        }
        insert_at = before ? vi_cur_row : (vi_cur_row + 1);
        for (i = 0; i < vi_yank_num_lines; i++) {
            if (vi_num_lines >= VI_MAX_LINES) {
                break;
            }
            vi_insert_line_at(insert_at + i);
            vi_memcpy(vi_lines[insert_at + i], vi_yank_lines[i], VI_LINE_MAX + 1);
        }
        vi_cur_row = insert_at;
        if (vi_cur_row >= vi_num_lines) {
            vi_cur_row = vi_num_lines - 1;
        }
        vi_cur_col = 0;
        vi_dirty = TRUE;
    } else {
        int32_t len;
        int32_t i;

        if (vi_yank_char_len <= 0) {
            return;
        }
        len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
        if (!before && len > 0) {
            vi_cur_col++;
            if (vi_cur_col > len) {
                vi_cur_col = len;
            }
        }
        for (i = 0; i < vi_yank_char_len; i++) {
            char c = vi_yank_chars[i];
            if (c == '\n') {
                vi_split_line_at_cursor();
            } else {
                vi_insert_char(c);
            }
        }
        if (vi_cur_col > 0) {
            vi_cur_col--;
        }
        vi_dirty = TRUE;
    }
}

static void vi_join_lines(int32_t row)
{
    char *cur;
    const char *src;
    int32_t cur_len;
    int32_t src_len;
    int32_t join_col;
    bool_t need_space;

    if (row < 0 || row + 1 >= vi_num_lines) {
        return;
    }

    cur     = vi_lines[row];
    cur_len = (int32_t)vi_strlen(cur);
    src     = vi_lines[row + 1];

    while (*src == ' ' || *src == '\t') {
        src++;
    }
    src_len = (int32_t)vi_strlen(src);

    join_col   = cur_len;
    need_space = (cur_len > 0 && src_len > 0 &&
                  cur[cur_len - 1] != ' ' && cur[cur_len - 1] != '\t') ? TRUE : FALSE;

    if (need_space && cur_len < VI_LINE_MAX) {
        cur[cur_len++] = ' ';
        cur[cur_len]   = '\0';
    }
    if (src_len > 0 && cur_len < VI_LINE_MAX) {
        int32_t copy_len = src_len;
        if (cur_len + copy_len > VI_LINE_MAX) {
            copy_len = VI_LINE_MAX - cur_len;
        }
        vi_memcpy(&cur[cur_len], src, (size_t)copy_len);
        cur[cur_len + copy_len] = '\0';
    }

    vi_delete_line_at(row + 1);
    vi_cur_row = row;
    vi_cur_col = join_col;
    vi_dirty   = TRUE;
}

static char *vi_emit_csi_pos(char *p, char *end, int32_t row0, int32_t col0)
{
    p = vi_fmt_str(p, end, "\x1b[");
    p = vi_fmt_uint(p, end, (uint32_t)(row0 + 1));
    p = vi_fmt_str(p, end, ";");
    p = vi_fmt_uint(p, end, (uint32_t)(col0 + 1));
    p = vi_fmt_str(p, end, "H");
    return p;
}

static void vi_render(void)
{
    int32_t vrow;
    char status[CON_COLS + 1];
    char *fp = vi_frame_buf;
    char *fend = vi_frame_buf + sizeof(vi_frame_buf) - 1U;
    bool_t in_visual = (vi_mode == VI_MODE_VISUAL || vi_mode == VI_MODE_VLINE) ? TRUE : FALSE;
    int32_t sr = 0, sc = 0, er = 0, ec = 0;

    vi_clamp_cursor();
    if (in_visual) {
        vi_get_visual_range(&sr, &sc, &er, &ec);
    }

    for (vrow = 0; vrow < VI_VIEW_ROWS; vrow++) {
        int32_t file_row = vi_top_row + vrow;
        fp = vi_emit_csi_pos(fp, fend, vrow, 0);
        fp = vi_fmt_str(fp, fend, "\x1b[0m\x1b[2K");

        if (file_row < vi_num_lines) {
            const char *line = vi_lines[file_row];
            if (in_visual && file_row >= sr && file_row <= er) {
                int32_t len = (int32_t)vi_strlen(line);
                int32_t max_col = (len > 0) ? len : 1;
                int32_t col;
                bool_t cur_hl = FALSE;
                for (col = 0; col < max_col && col < CON_COLS; col++) {
                    bool_t sel = vi_is_cell_selected(file_row, col, sr, sc, er, ec);
                    char ch = (col < len) ? line[col] : ' ';
                    if (sel != cur_hl) {
                        fp = vi_fmt_str(fp, fend, sel ? "\x1b[7m" : "\x1b[0m");
                        cur_hl = sel;
                    }
                    if (fp < fend) {
                        *fp++ = ch;
                    }
                }
                if (cur_hl) {
                    fp = vi_fmt_str(fp, fend, "\x1b[0m");
                }
            } else {
                fp = vi_fmt_str(fp, fend, line);
            }
        } else {
            if (fp < fend) {
                *fp++ = '~';
            }
        }
    }

    fp = vi_emit_csi_pos(fp, fend, CON_ROWS - 1, 0);
    fp = vi_fmt_str(fp, fend, "\x1b[0m\x1b[2K");

    if (vi_mode == VI_MODE_COMMAND) {
        if (fp < fend) {
            *fp++ = ':';
        }
        fp = vi_fmt_str(fp, fend, vi_cmd_buf);
        fp = vi_emit_csi_pos(fp, fend, CON_ROWS - 1, 1 + vi_cmd_len);
    } else {
        char *sp = status;
        char *send = status + sizeof(status) - 1U;
        if (vi_mode == VI_MODE_INSERT) {
            sp = vi_fmt_str(sp, send, "-- INSERT -- ");
        } else if (vi_mode == VI_MODE_VISUAL) {
            sp = vi_fmt_str(sp, send, "-- VISUAL -- ");
        } else if (vi_mode == VI_MODE_VLINE) {
            sp = vi_fmt_str(sp, send, "-- VISUAL LINE -- ");
        } else {
            sp = vi_fmt_str(sp, send, "[NORMAL] ");
        }
        if (vi_status_msg[0] != '\0') {
            sp = vi_fmt_str(sp, send, vi_status_msg);
        } else {
            sp = vi_fmt_str(sp, send, vi_filepath);
            if (vi_dirty) {
                sp = vi_fmt_str(sp, send, " [+]");
            }
        }
        sp = vi_fmt_str(sp, send, "  Ln ");
        sp = vi_fmt_uint(sp, send, (uint32_t)(vi_cur_row + 1));
        sp = vi_fmt_str(sp, send, ", Col ");
        sp = vi_fmt_uint(sp, send, (uint32_t)(vi_cur_col + 1));

        fp = vi_fmt_str(fp, fend, status);
        fp = vi_emit_csi_pos(fp, fend, vi_cur_row - vi_top_row, vi_cur_col);
    }

    /* Flush frame to VGA console via ANSI show-cursor / refresh sequence */
    fp = vi_fmt_str(fp, fend, "\x1b[?25h");
    (void)sys_write(1, vi_frame_buf, (uint32_t)(fp - vi_frame_buf));
}

static int vi_execute_ex_cmd(void)
{
    const char *cmd = vi_cmd_buf;

    while (*cmd == ' ') {
        cmd++;
    }
    if (*cmd == '\0') {
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }

    if (vi_strcmp(cmd, "w") == 0) {
        (void)vi_save_file(vi_filepath);
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (vi_strncmp(cmd, "w ", 2U) == 0) {
        const char *arg = &cmd[2];
        while (*arg == ' ') {
            arg++;
        }
        if (*arg != '\0') {
            size_t len = vi_strlen(arg);
            if (len < sizeof(vi_filepath)) {
                vi_memcpy(vi_filepath, arg, len + 1U);
            }
        }
        (void)vi_save_file(vi_filepath);
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (vi_strcmp(cmd, "wq") == 0 || vi_strcmp(cmd, "wq!") == 0 || vi_strcmp(cmd, "x") == 0) {
        if (vi_save_file(vi_filepath) == 0) {
            return 1;
        }
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (vi_strcmp(cmd, "q!") == 0) {
        return 1;
    }
    if (vi_strcmp(cmd, "q") == 0) {
        if (vi_dirty) {
            (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                             "E37: No write since last change (add ! to override)");
            vi_mode = VI_MODE_NORMAL;
            return 0;
        }
        return 1;
    }

    (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                     "E492: Not an editor command");
    vi_mode = VI_MODE_NORMAL;
    return 0;
}

static int vi_handle_key(char ch)
{
    if (vi_mode == VI_MODE_INSERT) {
        if (ch == 27) {
            vi_mode = VI_MODE_NORMAL;
            if (vi_cur_col > 0) {
                vi_cur_col--;
            }
            vi_status_msg[0] = '\0';
        } else if (ch == '\n') {
            vi_split_line_at_cursor();
        } else if (ch == '\b' || ch == 127) {
            vi_backspace_in_insert();
        } else if (ch >= 32 && ch <= 126) {
            vi_insert_char(ch);
        }
        return 0;
    }

    if (vi_mode == VI_MODE_COMMAND) {
        if (ch == 27) {
            vi_mode = VI_MODE_NORMAL;
            vi_cmd_len = 0;
            vi_cmd_buf[0] = '\0';
        } else if (ch == '\n') {
            vi_cmd_buf[vi_cmd_len] = '\0';
            return vi_execute_ex_cmd();
        } else if (ch == '\b' || ch == 127) {
            if (vi_cmd_len > 0) {
                vi_cmd_len--;
                vi_cmd_buf[vi_cmd_len] = '\0';
            } else {
                vi_mode = VI_MODE_NORMAL;
            }
        } else if (ch >= 32 && ch <= 126 && vi_cmd_len + 1 < (int32_t)sizeof(vi_cmd_buf)) {
            vi_cmd_buf[vi_cmd_len++] = ch;
            vi_cmd_buf[vi_cmd_len] = '\0';
        }
        return 0;
    }

    if (vi_mode == VI_MODE_VISUAL || vi_mode == VI_MODE_VLINE) {
        vi_status_msg[0] = '\0';
        switch (ch) {
        case 27:
            vi_mode = VI_MODE_NORMAL;
            break;
        case 'v':
            vi_mode = (vi_mode == VI_MODE_VISUAL) ? VI_MODE_NORMAL : VI_MODE_VISUAL;
            break;
        case 'V':
            vi_mode = (vi_mode == VI_MODE_VLINE) ? VI_MODE_NORMAL : VI_MODE_VLINE;
            break;
        case 'h':
            if (vi_cur_col > 0) {
                vi_cur_col--;
            }
            break;
        case 'j':
            if (vi_cur_row + 1 < vi_num_lines) {
                vi_cur_row++;
            }
            break;
        case 'k':
            if (vi_cur_row > 0) {
                vi_cur_row--;
            }
            break;
        case 'l':
            if (vi_cur_col + 1 < (int32_t)vi_strlen(vi_lines[vi_cur_row])) {
                vi_cur_col++;
            }
            break;
        case '0':
            vi_cur_col = 0;
            break;
        case '$': {
            int32_t len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
            vi_cur_col = (len > 0) ? (len - 1) : 0;
            break;
        }
        case 'g':
            vi_cur_row = 0;
            vi_cur_col = 0;
            break;
        case 'G':
            vi_cur_row = vi_num_lines - 1;
            vi_cur_col = 0;
            break;
        case 'y': {
            int32_t sr, sc, er, ec;
            vi_get_visual_range(&sr, &sc, &er, &ec);
            if (vi_mode == VI_MODE_VLINE) {
                vi_yank_line_range(sr, er);
                vi_cur_row = sr;
                vi_cur_col = 0;
            } else {
                vi_yank_char_range(sr, sc, er, ec);
                vi_cur_row = sr;
                vi_cur_col = sc;
            }
            vi_mode = VI_MODE_NORMAL;
            break;
        }
        case 'd':
        case 'x':
            vi_delete_visual_selection();
            vi_mode = VI_MODE_NORMAL;
            break;
        case 'J': {
            int32_t sr, sc, er, ec;
            int32_t joins;
            int32_t i;
            vi_get_visual_range(&sr, &sc, &er, &ec);
            joins = (er > sr) ? (er - sr) : 1;
            for (i = 0; i < joins; i++) {
                vi_join_lines(sr);
            }
            vi_mode = VI_MODE_NORMAL;
            break;
        }
        default:
            break;
        }
        return 0;
    }

    /* VI_MODE_NORMAL */
    vi_status_msg[0] = '\0';

    if (vi_pending_d) {
        vi_pending_d = FALSE;
        if (ch == 'd') {
            vi_yank_line_range(vi_cur_row, vi_cur_row);
            vi_delete_line_at(vi_cur_row);
        }
        return 0;
    }

    if (vi_pending_y) {
        vi_pending_y = FALSE;
        if (ch == 'y') {
            vi_yank_line_range(vi_cur_row, vi_cur_row);
        } else if (ch == 'j') {
            vi_yank_line_range(vi_cur_row, vi_cur_row + 1);
        } else if (ch == 'k') {
            vi_yank_line_range((vi_cur_row > 0) ? (vi_cur_row - 1) : 0, vi_cur_row);
        } else if (ch == '$') {
            int32_t len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
            if (len > 0 && vi_cur_col < len) {
                vi_yank_char_range(vi_cur_row, vi_cur_col, vi_cur_row, len - 1);
            }
        } else if (ch == '0') {
            if (vi_cur_col > 0) {
                vi_yank_char_range(vi_cur_row, 0, vi_cur_row, vi_cur_col - 1);
            }
        } else if (ch == 'l') {
            int32_t len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
            if (len > 0 && vi_cur_col < len) {
                vi_yank_char_range(vi_cur_row, vi_cur_col, vi_cur_row, vi_cur_col);
            }
        }
        return 0;
    }

    switch (ch) {
    case 'h':
        if (vi_cur_col > 0) {
            vi_cur_col--;
        }
        break;
    case 'j':
        if (vi_cur_row + 1 < vi_num_lines) {
            vi_cur_row++;
        }
        break;
    case 'k':
        if (vi_cur_row > 0) {
            vi_cur_row--;
        }
        break;
    case 'l':
        if (vi_cur_col + 1 < (int32_t)vi_strlen(vi_lines[vi_cur_row])) {
            vi_cur_col++;
        }
        break;
    case '0':
        vi_cur_col = 0;
        break;
    case '$': {
        int32_t len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
        vi_cur_col = (len > 0) ? (len - 1) : 0;
        break;
    }
    case 'g':
        vi_cur_row = 0;
        vi_cur_col = 0;
        break;
    case 'G':
        vi_cur_row = vi_num_lines - 1;
        vi_cur_col = 0;
        break;
    case 'i':
        vi_mode = VI_MODE_INSERT;
        break;
    case 'a': {
        int32_t len = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
        if (vi_cur_col < len) {
            vi_cur_col++;
        }
        vi_mode = VI_MODE_INSERT;
        break;
    }
    case 'A':
        vi_cur_col = (int32_t)vi_strlen(vi_lines[vi_cur_row]);
        vi_mode = VI_MODE_INSERT;
        break;
    case 'o':
        vi_insert_line_at(vi_cur_row + 1);
        vi_cur_row++;
        vi_cur_col = 0;
        vi_mode = VI_MODE_INSERT;
        break;
    case 'O':
        vi_insert_line_at(vi_cur_row);
        vi_cur_col = 0;
        vi_mode = VI_MODE_INSERT;
        break;
    case 'v':
        vi_mode    = VI_MODE_VISUAL;
        vi_vis_row = vi_cur_row;
        vi_vis_col = vi_cur_col;
        break;
    case 'V':
        vi_mode    = VI_MODE_VLINE;
        vi_vis_row = vi_cur_row;
        vi_vis_col = 0;
        break;
    case 'y':
        vi_pending_y = TRUE;
        break;
    case 'Y':
        vi_yank_line_range(vi_cur_row, vi_cur_row);
        break;
    case 'p':
        vi_paste(FALSE);
        break;
    case 'P':
        vi_paste(TRUE);
        break;
    case 'J':
        vi_join_lines(vi_cur_row);
        break;
    case 'x':
        vi_delete_char_at(vi_cur_row, vi_cur_col);
        break;
    case 'D':
        vi_lines[vi_cur_row][vi_cur_col] = '\0';
        vi_dirty = TRUE;
        break;
    case 'd':
        vi_pending_d = TRUE;
        break;
    case ':':
        vi_mode = VI_MODE_COMMAND;
        vi_cmd_len = 0;
        vi_cmd_buf[0] = '\0';
        break;
    default:
        break;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct vi_mouse_state last_mouse;
    struct vi_mouse_state cur_mouse;
    int mouse_fd;

    vi_filepath[0]    = '\0';
    vi_status_msg[0]  = '\0';
    vi_mode           = VI_MODE_NORMAL;
    vi_yank_linewise  = FALSE;
    vi_yank_num_lines = 0;
    vi_yank_char_len  = 0;

    if (argc > 1 && argv[1] != NULL) {
        size_t len = vi_strlen(argv[1]);
        if (len >= sizeof(vi_filepath)) {
            len = sizeof(vi_filepath) - 1U;
        }
        vi_memcpy(vi_filepath, argv[1], len);
        vi_filepath[len] = '\0';
    } else {
        (void)vi_fmt_str(vi_filepath, vi_filepath + sizeof(vi_filepath) - 1U, "/tmp/untitled.txt");
    }

    vi_serial_fd = sys_open("/dev/ttyS0", O_WRONLY, 0U);
    mouse_fd     = sys_open("/dev/mouse", O_RDONLY, 0U);

    /* Enter alternate screen buffer (ANSI/VT100) */
    (void)sys_write(1, "\x1b[?1049h", 8U);

    vi_load_file(vi_filepath);
    vi_memset(&last_mouse, 0, sizeof(last_mouse));
    if (mouse_fd >= 0) {
        (void)sys_read(mouse_fd, &last_mouse, sizeof(last_mouse));
    }
    vi_render();

    for (;;) {
        uint8_t ch;
        int need_redraw = 0;

        while (sys_read(0, &ch, 1U) == 1) {
            if (vi_handle_key((char)ch)) {
                (void)sys_write(1, "\x1b[?1049l", 8U);
                if (mouse_fd >= 0) {
                    (void)sys_close(mouse_fd);
                }
                if (vi_serial_fd >= 0) {
                    (void)sys_close(vi_serial_fd);
                }
                return 0;
            }
            need_redraw = 1;
        }

        if (mouse_fd >= 0 && sys_read(mouse_fd, &cur_mouse, sizeof(cur_mouse)) == (int32_t)sizeof(cur_mouse)) {
            if (cur_mouse.x != last_mouse.x ||
                cur_mouse.y != last_mouse.y ||
                cur_mouse.buttons != last_mouse.buttons ||
                cur_mouse.packets != last_mouse.packets) {
                last_mouse = cur_mouse;
                need_redraw = 1;
            }
        }

        if (need_redraw) {
            vi_render();
        } else {
            sys_sleep_ms(2);
        }
    }
}
