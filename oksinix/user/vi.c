/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - /bin/vi (Minimal modal full-screen text editor)
 */

#include "user.h"
#include "vfs.h"
#include "xhci.h"
#include "console.h"
#include "ps2.h"
#include "io.h"
#include "serial.h"
#include "string.h"

#define VI_MAX_LINES  64
#define VI_LINE_MAX   (CON_COLS - 1)
#define VI_VIEW_ROWS  (CON_ROWS - 1)

#define VI_MODE_NORMAL  0
#define VI_MODE_INSERT  1
#define VI_MODE_COMMAND 2
#define VI_MODE_VISUAL  3
#define VI_MODE_VLINE   4

#define VI_YANK_CHAR_MAX (VI_MAX_LINES * (VI_LINE_MAX + 1))

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

static char    vi_filepath[VFS_MAX_PATH];
static char    vi_cmd_buf[64];
static int32_t vi_cmd_len   = 0;
static char    vi_status_msg[CON_COLS + 1];

static char *vi_fmt_uint(char *dst, char *end, uint32_t val)
{
    char tmp[16];
    int i = 0;
    if (dst == NULL || end == NULL || dst > end) {
        return dst;
    }
    if (val == 0) {
        if (dst < end) {
            *dst++ = '0';
        }
    } else {
        while (val > 0) {
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

static void vi_clamp_cursor(void)
{
    int32_t len;

    if (vi_cur_row < 0) {
        vi_cur_row = 0;
    }
    if (vi_cur_row >= vi_num_lines) {
        vi_cur_row = vi_num_lines - 1;
    }

    len = (int32_t)strlen(vi_lines[vi_cur_row]);
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
    uint32_t total_bytes = 0;

    memset(vi_lines, 0, sizeof(vi_lines));
    vi_num_lines  = 1;
    vi_cur_row    = 0;
    vi_cur_col    = 0;
    vi_top_row    = 0;
    vi_dirty      = FALSE;
    vi_pending_d  = FALSE;
    vi_pending_y  = FALSE;

    fd = vfs_open(path, VFS_O_RDONLY);
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

        while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
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
        (void)vfs_close(fd);

        /* If file ended with a newline, don't count trailing empty slot as an extra line */
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

    kprintf("[VI] loaded \"%s\" (%u lines, %u bytes)\n",
            path, (uint32_t)vi_num_lines, total_bytes);
}

static int vi_save_file(const char *path)
{
    char serial_buf[VFS_MAX_FILE_SZ];
    uint32_t pos = 0;
    int32_t r;
    int fd;

    if (path == NULL || path[0] == '\0') {
        (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                         "E32: No file name");
        return -1;
    }

    for (r = 0; r < vi_num_lines; r++) {
        size_t len = strlen(vi_lines[r]);
        if (pos + (uint32_t)len + 1U < sizeof(serial_buf)) {
            memcpy(&serial_buf[pos], vi_lines[r], len);
            pos += (uint32_t)len;
            serial_buf[pos++] = '\n';
        }
    }

    fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        (void)vi_fmt_str(vi_status_msg, vi_status_msg + sizeof(vi_status_msg) - 1U,
                         "E212: Cannot open file for writing");
        return -1;
    }

    if (pos > 0) {
        (void)vfs_write(fd, serial_buf, pos);
    }
    (void)vfs_close(fd);

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

    kprintf("[VI] saved \"%s\" (%u lines, %u bytes)\n",
            path, (uint32_t)vi_num_lines, pos);
    return 0;
}

static void vi_insert_line_at(int32_t idx)
{
    int32_t r;
    if (vi_num_lines >= VI_MAX_LINES) {
        return;
    }
    for (r = vi_num_lines; r > idx; r--) {
        memcpy(vi_lines[r], vi_lines[r - 1], VI_LINE_MAX + 1);
    }
    memset(vi_lines[idx], 0, VI_LINE_MAX + 1);
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
        memcpy(vi_lines[r], vi_lines[r + 1], VI_LINE_MAX + 1);
    }
    memset(vi_lines[vi_num_lines - 1], 0, VI_LINE_MAX + 1);
    vi_num_lines--;
    if (vi_cur_row >= vi_num_lines) {
        vi_cur_row = vi_num_lines - 1;
    }
    vi_dirty = TRUE;
}

static void vi_insert_char(char c)
{
    char *line = vi_lines[vi_cur_row];
    int32_t len = (int32_t)strlen(line);
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
    int32_t len = (int32_t)strlen(line);
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
    size_t tail_len = strlen(&line[vi_cur_col]);

    if (vi_num_lines >= VI_MAX_LINES) {
        return;
    }
    memcpy(tail, &line[vi_cur_col], tail_len + 1U);
    line[vi_cur_col] = '\0';

    vi_insert_line_at(vi_cur_row + 1);
    memcpy(vi_lines[vi_cur_row + 1], tail, tail_len + 1U);
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
        /* Join current line onto end of previous line */
        int32_t prev_len = (int32_t)strlen(vi_lines[vi_cur_row - 1]);
        int32_t cur_len  = (int32_t)strlen(vi_lines[vi_cur_row]);
        int32_t copy_len = cur_len;

        if (prev_len + copy_len > VI_LINE_MAX) {
            copy_len = VI_LINE_MAX - prev_len;
        }
        if (copy_len > 0) {
            memcpy(&vi_lines[vi_cur_row - 1][prev_len], vi_lines[vi_cur_row], (size_t)copy_len);
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
        int32_t end_len = (int32_t)strlen(vi_lines[*er]);
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
        memcpy(vi_yank_lines[cnt], vi_lines[r], VI_LINE_MAX + 1);
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
        int32_t len = (int32_t)strlen(vi_lines[r]);
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
        int32_t len = (int32_t)strlen(line);
        if (len > 0 && sc < len) {
            int32_t del_end = (ec < len) ? ec : (len - 1);
            int32_t tail_start = del_end + 1;
            int32_t tail_len = len - tail_start;
            if (tail_len > 0) {
                memcpy(&line[sc], &line[tail_start], (size_t)tail_len + 1U);
            } else {
                line[sc] = '\0';
            }
            vi_dirty = TRUE;
        }
    } else {
        int32_t end_len = (int32_t)strlen(vi_lines[er]);
        int32_t tail_start = ec + 1;
        int32_t tail_len = (tail_start < end_len) ? (end_len - tail_start) : 0;
        int32_t copy_len = tail_len;
        int32_t count = er - sr;
        int32_t i;

        if (sc > (int32_t)strlen(vi_lines[sr])) {
            sc = (int32_t)strlen(vi_lines[sr]);
        }
        if (sc + copy_len > VI_LINE_MAX) {
            copy_len = VI_LINE_MAX - sc;
        }
        if (copy_len > 0) {
            memcpy(&vi_lines[sr][sc], &vi_lines[er][tail_start], (size_t)copy_len);
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
            memcpy(vi_lines[insert_at + i], vi_yank_lines[i], VI_LINE_MAX + 1);
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
        len = (int32_t)strlen(vi_lines[vi_cur_row]);
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
    cur_len = (int32_t)strlen(cur);
    src     = vi_lines[row + 1];

    /* Strip leading spaces/tabs from the joined line (standard vi J behavior) */
    while (*src == ' ' || *src == '\t') {
        src++;
    }
    src_len = (int32_t)strlen(src);

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
        memcpy(&cur[cur_len], src, (size_t)copy_len);
        cur[cur_len + copy_len] = '\0';
    }

    vi_delete_line_at(row + 1);
    vi_cur_row = row;
    vi_cur_col = join_col;
    vi_dirty   = TRUE;
}

static void vi_render(void)
{
    int32_t vrow;
    char status[CON_COLS + 1];
    char *p;
    bool_t in_visual = (vi_mode == VI_MODE_VISUAL || vi_mode == VI_MODE_VLINE) ? TRUE : FALSE;
    int32_t sr = 0, sc = 0, er = 0, ec = 0;

    vi_clamp_cursor();
    if (in_visual) {
        vi_get_visual_range(&sr, &sc, &er, &ec);
    }

    for (vrow = 0; vrow < VI_VIEW_ROWS; vrow++) {
        int32_t file_row = vi_top_row + vrow;
        console_clear_row(vrow);
        if (file_row < vi_num_lines) {
            console_puts_at(0, vrow, vi_lines[file_row]);
            if (in_visual && file_row >= sr && file_row <= er) {
                int32_t len = (int32_t)strlen(vi_lines[file_row]);
                int32_t max_col = (len > 0) ? len : 1;
                int32_t col;
                for (col = 0; col < max_col && col < CON_COLS; col++) {
                    if (vi_is_cell_selected(file_row, col, sr, sc, er, ec)) {
                        console_set_highlight(col, vrow, TRUE);
                    }
                }
            }
        } else {
            console_put_at(0, vrow, '~');
        }
    }

    console_clear_row(CON_ROWS - 1);
    if (vi_mode == VI_MODE_COMMAND) {
        console_put_at(0, CON_ROWS - 1, ':');
        console_puts_at(1, CON_ROWS - 1, vi_cmd_buf);
        console_set_cursor(1 + vi_cmd_len, CON_ROWS - 1);
    } else {
        char *end = status + sizeof(status) - 1U;
        p = status;
        if (vi_mode == VI_MODE_INSERT) {
            p = vi_fmt_str(p, end, "-- INSERT -- ");
        } else if (vi_mode == VI_MODE_VISUAL) {
            p = vi_fmt_str(p, end, "-- VISUAL -- ");
        } else if (vi_mode == VI_MODE_VLINE) {
            p = vi_fmt_str(p, end, "-- VISUAL LINE -- ");
        } else {
            p = vi_fmt_str(p, end, "[NORMAL] ");
        }
        if (vi_status_msg[0] != '\0') {
            p = vi_fmt_str(p, end, vi_status_msg);
        } else {
            p = vi_fmt_str(p, end, vi_filepath);
            if (vi_dirty) {
                p = vi_fmt_str(p, end, " [+]");
            }
        }
        p = vi_fmt_str(p, end, "  Ln ");
        p = vi_fmt_uint(p, end, (uint32_t)(vi_cur_row + 1));
        p = vi_fmt_str(p, end, ", Col ");
        p = vi_fmt_uint(p, end, (uint32_t)(vi_cur_col + 1));

        console_puts_at(0, CON_ROWS - 1, status);
        console_set_cursor(vi_cur_col, vi_cur_row - vi_top_row);
    }

    console_refresh();
}

/* Execute ':w', ':q', ':wq', ':q!', ':x' command; returns 1 if editor should exit */
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

    if (strcmp(cmd, "w") == 0) {
        (void)vi_save_file(vi_filepath);
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (strncmp(cmd, "w ", 2) == 0) {
        const char *arg = &cmd[2];
        while (*arg == ' ') {
            arg++;
        }
        if (*arg != '\0') {
            size_t len = strlen(arg);
            if (len < sizeof(vi_filepath)) {
                memcpy(vi_filepath, arg, len + 1U);
            }
        }
        (void)vi_save_file(vi_filepath);
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "wq!") == 0 || strcmp(cmd, "x") == 0) {
        if (vi_save_file(vi_filepath) == 0) {
            return 1;
        }
        vi_mode = VI_MODE_NORMAL;
        return 0;
    }
    if (strcmp(cmd, "q!") == 0) {
        return 1;
    }
    if (strcmp(cmd, "q") == 0) {
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
        if (ch == 27) { /* ESC */
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
        if (ch == 27) { /* ESC cancels command mode */
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
        case 27: /* ESC */
            vi_mode = VI_MODE_NORMAL;
            break;
        case 'v':
            if (vi_mode == VI_MODE_VISUAL) {
                vi_mode = VI_MODE_NORMAL;
            } else {
                vi_mode = VI_MODE_VISUAL;
            }
            break;
        case 'V':
            if (vi_mode == VI_MODE_VLINE) {
                vi_mode = VI_MODE_NORMAL;
            } else {
                vi_mode = VI_MODE_VLINE;
            }
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
            if (vi_cur_col + 1 < (int32_t)strlen(vi_lines[vi_cur_row])) {
                vi_cur_col++;
            }
            break;
        case '0':
            vi_cur_col = 0;
            break;
        case '$': {
            int32_t len = (int32_t)strlen(vi_lines[vi_cur_row]);
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
            return 0;
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
            int32_t len = (int32_t)strlen(vi_lines[vi_cur_row]);
            if (len > 0 && vi_cur_col < len) {
                vi_yank_char_range(vi_cur_row, vi_cur_col, vi_cur_row, len - 1);
            }
        } else if (ch == '0') {
            if (vi_cur_col > 0) {
                vi_yank_char_range(vi_cur_row, 0, vi_cur_row, vi_cur_col - 1);
            }
        } else if (ch == 'l') {
            int32_t len = (int32_t)strlen(vi_lines[vi_cur_row]);
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
        if (vi_cur_col + 1 < (int32_t)strlen(vi_lines[vi_cur_row])) {
            vi_cur_col++;
        }
        break;
    case '0':
        vi_cur_col = 0;
        break;
    case '$': {
        int32_t len = (int32_t)strlen(vi_lines[vi_cur_row]);
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
        int32_t len = (int32_t)strlen(vi_lines[vi_cur_row]);
        if (vi_cur_col < len) {
            vi_cur_col++;
        }
        vi_mode = VI_MODE_INSERT;
        break;
    }
    case 'A':
        vi_cur_col = (int32_t)strlen(vi_lines[vi_cur_row]);
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

int bin_vi_main(int argc, char **argv)
{
    struct mouse_state last_mouse;

    vi_filepath[0] = '\0';
    vi_status_msg[0] = '\0';
    vi_mode = VI_MODE_NORMAL;
    vi_yank_linewise  = FALSE;
    vi_yank_num_lines = 0;
    vi_yank_char_len  = 0;

    if (argc > 1 && argv[1] != NULL) {
        size_t len = strlen(argv[1]);
        if (len >= sizeof(vi_filepath)) {
            len = sizeof(vi_filepath) - 1U;
        }
        memcpy(vi_filepath, argv[1], len);
        vi_filepath[len] = '\0';
    } else {
        (void)vi_fmt_str(vi_filepath, vi_filepath + sizeof(vi_filepath) - 1U, "/tmp/untitled.txt");
    }

    console_save_screen();
    console_set_status("PID 2: /bin/vi (editing)", 0);

    vi_load_file(vi_filepath);
    mouse_get_state(&last_mouse);
    vi_render();

    for (;;) {
        uint8_t ch;
        struct mouse_state cur_mouse;
        int need_redraw = 0;

        while (vfs_read(0, &ch, 1) == 1) {
            if (vi_handle_key((char)ch)) {
                console_restore_screen();
                console_set_status("PID 1: /bin/init -> PID 2: /bin/sh", 0);
                console_refresh();
                return 0;
            }
            need_redraw = 1;
        }

        mouse_get_state(&cur_mouse);
        if (cur_mouse.x != last_mouse.x ||
            cur_mouse.y != last_mouse.y ||
            cur_mouse.buttons != last_mouse.buttons ||
            cur_mouse.packets != last_mouse.packets) {
            last_mouse = cur_mouse;
            need_redraw = 1;
        }

        if (need_redraw) {
            vi_render();
        }

        if (xhci_is_initialized()) {
            xhci_poll();
            io_wait();
        } else {
            cpu_hlt();
        }
    }
}
