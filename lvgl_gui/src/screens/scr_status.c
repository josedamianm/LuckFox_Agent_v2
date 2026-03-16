#include "scr_status.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *scr;
static lv_obj_t *lbl_ip;
static lv_obj_t *lbl_cpu_val;
static lv_obj_t *lbl_mem_val;
static lv_obj_t *bar_cpu;
static lv_obj_t *bar_mem;

static void read_ip(char *out, int sz) {
    FILE *f = fopen("/proc/net/fib_trie", "r");
    strncpy(out, "---", sz);
    if (!f) return;
    char line[128];
    int found_local = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "LOCAL")) { found_local = 1; continue; }
        if (found_local) {
            char *p = strstr(line, "32 host");
            if (p) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                char *e = s;
                while (*e && *e != '\n' && *e != ' ') e++;
                *e = '\0';
                if (strncmp(s, "127.", 4) != 0 && strlen(s) > 6) {
                    strncpy(out, s, sz - 1);
                    out[sz - 1] = '\0';
                    break;
                }
            }
            found_local = 0;
        }
    }
    fclose(f);
}

static int read_cpu_percent(void) {
    static long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    long user, nice, sys, idle, iow, irq, sirq;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &sys, &idle, &iow, &irq, &sirq) != 7) {
        fclose(f); return 0;
    }
    fclose(f);
    long total = user + nice + sys + idle + iow + irq + sirq;
    long dt = total - prev_total;
    long di = idle - prev_idle;
    prev_total = total; prev_idle = idle;
    if (dt == 0) return 0;
    return (int)(100 * (dt - di) / dt);
}

static int read_mem_percent(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    long total = 0, avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld", &avail) == 1) break;
    }
    fclose(f);
    if (total == 0) return 0;
    return (int)(100 * (total - avail) / total);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    char ip[32];
    read_ip(ip, sizeof(ip));
    lv_label_set_text(lbl_ip, ip);

    int cpu = read_cpu_percent();
    int mem = read_mem_percent();

    lv_label_set_text_fmt(lbl_cpu_val, "%d%%", cpu);
    lv_label_set_text_fmt(lbl_mem_val, "%d%%", mem);
    lv_bar_set_value(bar_cpu, cpu, LV_ANIM_ON);
    lv_bar_set_value(bar_mem, mem, LV_ANIM_ON);
}

/*
 * Layout (240×240):
 *
 *   "LUCKFOX AGENT"  — 16px, muted gray, top-center  (y=14)
 *   IP address       — 32px, cyan, centered            (y=38)
 *
 *   CPU  [ bar ]  35%  — 12px label, 16px bar, 24px value  (y=100)
 *   MEM  [ bar ]  60%  — same                              (y=152)
 *
 *   "UPTIME: …"      — 12px, very muted, bottom            (y=220)
 */
void scr_status_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_make(0x0A, 0x0A, 0x1A), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ── Title label ── */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(lbl_title, "LUCKFOX AGENT");

    /* ── IP address (large, cyan) ── */
    lbl_ip = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_ip, lv_color_make(0x00, 0xEE, 0xFF), 0);
    lv_obj_align(lbl_ip, LV_ALIGN_TOP_MID, 0, 32);
    lv_label_set_text(lbl_ip, "...");

    /* ── CPU row ── */
    lv_obj_t *lbl_cpu_name = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_cpu_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_cpu_name, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_cpu_name, LV_ALIGN_TOP_LEFT, 12, 96);
    lv_label_set_text(lbl_cpu_name, "CPU");

    lbl_cpu_val = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_cpu_val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_cpu_val, lv_color_make(0x00, 0xFF, 0x88), 0);
    lv_obj_align(lbl_cpu_val, LV_ALIGN_TOP_RIGHT, -12, 90);
    lv_label_set_text(lbl_cpu_val, "0%");

    bar_cpu = lv_bar_create(scr);
    lv_obj_set_size(bar_cpu, 216, 10);
    lv_obj_align(bar_cpu, LV_ALIGN_TOP_MID, 0, 122);
    lv_bar_set_range(bar_cpu, 0, 100);
    lv_obj_set_style_bg_color(bar_cpu, lv_color_make(0x22, 0x22, 0x22), 0);
    lv_obj_set_style_bg_color(bar_cpu, lv_color_make(0x00, 0xFF, 0x88), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_cpu, 5, 0);
    lv_obj_set_style_radius(bar_cpu, 5, LV_PART_INDICATOR);

    /* ── MEM row ── */
    lv_obj_t *lbl_mem_name = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_mem_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mem_name, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_mem_name, LV_ALIGN_TOP_LEFT, 12, 148);
    lv_label_set_text(lbl_mem_name, "MEM");

    lbl_mem_val = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_mem_val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_mem_val, lv_color_make(0x44, 0xAA, 0xFF), 0);
    lv_obj_align(lbl_mem_val, LV_ALIGN_TOP_RIGHT, -12, 142);
    lv_label_set_text(lbl_mem_val, "0%");

    bar_mem = lv_bar_create(scr);
    lv_obj_set_size(bar_mem, 216, 10);
    lv_obj_align(bar_mem, LV_ALIGN_TOP_MID, 0, 174);
    lv_bar_set_range(bar_mem, 0, 100);
    lv_obj_set_style_bg_color(bar_mem, lv_color_make(0x22, 0x22, 0x22), 0);
    lv_obj_set_style_bg_color(bar_mem, lv_color_make(0x44, 0xAA, 0xFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_mem, 5, 0);
    lv_obj_set_style_radius(bar_mem, 5, LV_PART_INDICATOR);

    /* ── Bottom label ── */
    lv_obj_t *lbl_up = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_up, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_up, lv_color_make(0x77, 0x77, 0x77), 0);
    lv_obj_align(lbl_up, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(lbl_up, "LUCKFOX PICO MAX");

    lv_timer_create(timer_cb, 2000, NULL);
    timer_cb(NULL);
}

lv_obj_t *scr_status_get(void) {
    return scr;
}
