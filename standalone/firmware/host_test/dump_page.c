/* Writes the real setup page HTML (from formutil.c) with sample values, for preview screenshots. */
#include <stdio.h>
#include <stdlib.h>
#include "assist_util.h"
#include "formutil.h"
int main(int argc, char **argv)
{
    const char *scans[] = { "Gilliland-Home", "Gilliland-Guest", "NETGEAR42" };
    setup_page_info_t in = {
        .ssid = "Gilliland-Home", .wifi_connected = argc > 2, .ip = "192.168.1.50",
        .api_key_masked = argc > 2 ? "****9xQz" : "", .speech_key_masked = argc > 2 ? "****k7mQ" : "", .model = 0,
        .voice = AU_DEFAULT_VOICE, .volume = 70, .wake_enabled = true, .wake_word = AU_DEFAULT_WAKE, .scan_ssids = scans, .scan_n = 3,
        .last_q = argc > 2 ? "What should I grill tonight?" : NULL,
        .last_a = argc > 2 ? "Ribeyes. It's 68 and clear in Fort Worth - perfect grilling weather." : NULL,
        .notice = argc > 2 ? "Saved." : NULL,
    };
    char *page = malloc(20480);
    int n = net_setup_page_build(page, 20480, &in);
    if (n < 0) return 1;
    FILE *f = fopen(argv[1], "w");
    fwrite(page, 1, n, f);
    fclose(f);
    return 0;
}
