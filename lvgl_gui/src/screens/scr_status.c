#include "scr_status.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *scr;
static lv_obj_t *lbl_ip;
static lv_obj_t *lbl_time;
static lv_obj_t *bar_cpu;
static lv_obj_t *bar_mem;

static void read_ip(char *out, int sz) {
    FILE *f = popen("ip -4 addr show eth0 2>/dev/null | grep -oP 'inet \\K[0-9.]+'", "r");
    if (f) {
        if (fgets(out, sz, f) == NULL) strncpy(out, "---", sz);
        else { char *nl = strchr(out, '\n'); if (nl) *nl = '\0'; }
        pclose(f);
    } else {
        strncpy(out, "---", sz);
    }
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
    lv_label_set_text_fmt(lbl_ip, "IP: %s", ip);

    lv_bar_set_value(bar_cpu, read_cpu_percent(), LV_ANIM_ON);
    lv_bar_set_value(bar_mem, read_mem_percent(), LV_ANIM_ON);
}

void scr_status_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(lbl_time, "LuckFox Agent");

    lbl_ip = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_ip, lv_color_make(0x00, 0xCC, 0xFF), 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_ip, LV_ALIGN_TOP_MID, 0, 50);
    lv_label_set_text(lbl_ip, "IP: ...");

    lv_obj_t *lbl_cpu = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_cpu, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_cpu, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_cpu, LV_ALIGN_LEFT_MID, 10, 10);
    lv_label_set_text(lbl_cpu, "CPU");

    bar_cpu = lv_bar_create(scr);
    lv_obj_set_size(bar_cpu, 180, 16);
    lv_obj_align(bar_cpu, LV_ALIGN_LEFT_MID, 42, 10);
    lv_bar_set_range(bar_cpu, 0, 100);
    lv_obj_set_style_bg_color(bar_cpu, lv_color_make(0x00, 0x80, 0x00), LV_PART_INDICATOR);

    lv_obj_t *lbl_mem = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_mem, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_mem, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_mem, LV_ALIGN_LEFT_MID, 10, 40);
    lv_label_set_text(lbl_mem, "MEM");

    bar_mem = lv_bar_create(scr);
    lv_obj_set_size(bar_mem, 180, 16);
    lv_obj_align(bar_mem, LV_ALIGN_LEFT_MID, 42, 40);
    lv_bar_set_range(bar_mem, 0, 100);
    lv_obj_set_style_bg_color(bar_mem, lv_color_make(0x00, 0x60, 0xCC), LV_PART_INDICATOR);

    lv_timer_create(timer_cb, 2000, NULL);
    timer_cb(NULL);
}

lv_obj_t *scr_status_get(void) {
    return scr;
}
