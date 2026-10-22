#include <ncurses.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <locale.h> // Needed for printing Unicode solid blocks

#define BLOBS 5

/*
 * GLOBAL METABALL VISCOSITY
 * GLOBAL METABALL VISCOSITY
 * 0.0 = normal/full animation speed
 * 1.0 = half speed
 * 3.0 = quarter speed
 * 9.0 = one-tenth speed
 * 19.0 = one-twentieth speed
 *
 * Higher values continuously slow travel, pulsing, sway, and ripples
 * for ALL metaballs. This avoids an effect that is visible only at startup.
 */
static float viscosity = 0.0f;

/*
 * METABALL MERGE DISTANCE
 * 1.0 = current/default merge distance
 * Higher values make metaballs merge while farther apart.
 * Lower values make them stay separate until closer together.
 * This affects all metaballs and is independent of viscosity.
 */
static float merge_distance = 1.0f;

/*
 * INDEPENDENT METABALL MERGE / SEPARATION CYCLE
 * merge_frequency = cycles per second before viscosity is applied.
 * 0.05f = slow changes (about one cycle every 20 seconds)
 * 0.20f = moderate changes (about one cycle every 5 seconds)
 * 0.50f = fast changes (about one cycle every 2 seconds)
 *
 * Each metaball gets its own phase and slightly different rate, so they
 * stick together and pull apart independently rather than in unison.
 */
static float merge_frequency = 0.05f;

/* Amount of independent merge/separation pulsing: 0.0 to 0.8. */
static float merge_strength = 0.8f;

static float viscosity_speed(void)
{
    /* Higher viscosity continuously slows the entire metaball animation. */
    float v = viscosity;
    if (v < 0.0f) v = 0.0f;
    if (v > 50.0f) v = 50.0f;
    return 1.0f / (1.0f + v);
}

typedef struct {
    float base_x;       
    float x, y;
    float r;
    float pulse_speed;  
    float phase_shift;  
    float sway_speed;   
    float phase;
    float time_pos;        
    float total_path_time; 
    float travel_time;     
    float hold_time;       
    float merge_phase;
    float merge_rate;
} Blob;

void init_lava_colors() {
    start_color();

    /*
     * Use only the 240 extended-color slots (16..255), which avoids
     * invalid ncurses color/pair numbers on 256-color terminals.
     * The palette follows the reference image continuously:
     * burgundy -> deep red -> scarlet -> orange -> golden yellow -> ivory.
     */
    for (int i = 0; i < 240; i++) {
        float r, g, b;
        float t;

        if (i < 42) {
            t = i / 41.0f;
            r = 30.0f + t * 190.0f;
            g = 2.0f + t * 8.0f;
            b = 1.0f + t * 1.0f;
        } else if (i < 105) {
            t = (i - 42) / 63.0f;
            r = 220.0f + t * 780.0f;
            g = 10.0f + t * 65.0f;
            b = 2.0f + t * 1.0f;
        } else if (i < 165) {
            t = (i - 105) / 60.0f;
            r = 1000.0f;
            g = 75.0f + t * 340.0f;
            b = 3.0f + t * 8.0f;
        } else if (i < 210) {
            t = (i - 165) / 45.0f;
            r = 1000.0f;
            g = 415.0f + t * 430.0f;
            b = 11.0f + t * 120.0f;
        } else {
            t = (i - 210) / 29.0f;
            r = 1000.0f;
            g = 845.0f + t * 155.0f;
            b = 131.0f + t * 760.0f;
        }

        init_color((short)(i + 16), (short)r, (short)g, (short)b);
        init_pair((short)(i + 1), (short)(i + 16), (short)(i + 16));
    }
}
int main() {
    setlocale(LC_ALL, ""); // Initialize wide character support for solid blocks
    initscr();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    
    if (has_colors() == FALSE) {
        endwin();
        printf("Your terminal does not support color.\n");
        return 1;
    }
    
    init_lava_colors();

    int w, h;
    getmaxyx(stdscr, h, w);
    if (w <= 0) w = 80;
    if (h <= 0) h = 24;

    // FIX: Because each pixel is now 2 characters wide, we halve our working width bounds
    int render_w = w / 2; 

    Blob blobs[BLOBS];
    srand(time(NULL));
    for (int i = 0; i < BLOBS; i++) {
        if (render_w > 16) {
            blobs[i].base_x = 8 + (rand() % (render_w - 16));
        } else {
            blobs[i].base_x = render_w / 2;
        }
        blobs[i].x = blobs[i].base_x;
        blobs[i].y = rand() % h;
        
        // Balanced sizing radius for square rendering math
        blobs[i].r = 10.5f + (rand() % 2);
        
        blobs[i].pulse_speed = 0.05f + ((rand() % 100) / 100.0f) * 0.1f; 
        blobs[i].phase_shift = (rand() % 360) * (3.14159f / 180.0f);
        blobs[i].sway_speed = 0.5f + ((rand() % 100) / 100.0f) * 1.0f;
        blobs[i].merge_phase = ((rand() % 10000) / 10000.0f) * 6.2831853f;
        blobs[i].merge_rate = 0.70f + ((rand() % 100) / 100.0f) * 0.60f;

        blobs[i].travel_time = 4.0f + (rand() % 3);       
        blobs[i].hold_time = 5.0f + ((rand() % 50) / 10.0f); 
        
        blobs[i].total_path_time = (blobs[i].travel_time * 2.0f) + (blobs[i].hold_time * 2.0f);
        blobs[i].time_pos = ((rand() % 1000) / 1000.0f) * blobs[i].total_path_time; 
    }

    float t = 0;
    float dt = 0.033f; 
    float c = 0.0;
    float merge_clock = 0.0f;
    while (getch() == ERR) {
        float speed = viscosity_speed();
        /* Track the merge cycle continuously; viscosity slows it too. */
        merge_clock += 0.066f * speed;
        c += 20.0f * speed;
        t += 0.005f * speed;

        // Render pass over the square screen matrix
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < render_w; x++) {
                float sum = 0;
                
                for (int i = 0; i < BLOBS; i++) {
                    float dx = x - blobs[i].x;
                    float dy = y - blobs[i].y;
                    
                    // FIXED: Removed the squash factors entirely. Normal 1:1 circle distance.
                    float d2 = (dx * dx) + (dy * dy) + 0.1f;
                    float ripple = sinf(atan2f(dy, dx) * 4.0 + c + blobs[i].phase) * 0.3f;
                    float fluctuation = 1.5f + sinf((t * blobs[i].pulse_speed * 10.0f) + blobs[i].phase_shift) * 1.0f;
                    float dynamic_r = blobs[i].r + fluctuation + ripple;

                    /*
                     * merge_distance controls the overall joining distance.
                     * Each blob independently expands/contracts its field using
                     * its own phase and rate, making blobs stick together and
                     * pull apart at different times.
                     */
                    float md = merge_distance;
                    if (md < 0.1f) md = 0.1f;
                    if (md > 5.0f) md = 5.0f;

                    float strength = merge_strength;
                    if (strength < 0.0f) strength = 0.0f;
                    if (strength > 0.8f) strength = 0.8f;
                    float freq = merge_frequency;
                    if (freq < 0.0f) freq = 0.0f;
                    if (freq > 5.0f) freq = 5.0f;
                    float merge_wave = sinf(merge_clock * freq * 6.2831853f
                                            * blobs[i].merge_rate
                                            + blobs[i].merge_phase);
                    float merge_factor = 1.0f + strength * merge_wave;
                    sum += ((dynamic_r * dynamic_r) * (md * md)
                            * (merge_factor * merge_factor)) / d2;
                }

                int color_idx = (int)(sum * 45);
                if (color_idx > 254) color_idx = 254;
                if (color_idx < 0) color_idx = 0;

                // FIX: Print double solid blocks to form perfectly square pixel clusters
                int palette_idx = (color_idx * 239) / 254;
                attron(COLOR_PAIR(palette_idx + 1));
                mvaddstr(y, x * 2, "  ");
                attroff(COLOR_PAIR(palette_idx + 1));
            }
        }

        // Timeline position math
        for (int i = 0; i < BLOBS; i++) {
            float sway_offset = sinf((t * blobs[i].sway_speed) + blobs[i].phase_shift) * 2.0f;
            float target_x = blobs[i].base_x + sway_offset;
            blobs[i].x = target_x;

            /* Scale the whole travel/hold cycle, not just the initial easing. */
            blobs[i].time_pos += dt * speed;
            if (blobs[i].time_pos >= blobs[i].total_path_time) {
                blobs[i].time_pos -= blobs[i].total_path_time;
            }

            // FIX: Push targets completely off screen boundaries based on max blob sizes
            float top_y = -8.0f; 
            float bottom_y = h + 8.0f;
            float progress = 0.0f;
            float target_y;

            if (blobs[i].time_pos < blobs[i].travel_time) {
                // Moving Down
                float linear_t = blobs[i].time_pos / blobs[i].travel_time;
                progress = (1.0f - cosf(linear_t * 3.14159f)) / 2.0f; 
                target_y = top_y + progress * (bottom_y - top_y);
            } 
            else if (blobs[i].time_pos < (blobs[i].travel_time + blobs[i].hold_time)) {
                // Stuck completely hidden past the bottom edge
                target_y = bottom_y;
            } 
            else if (blobs[i].time_pos < (blobs[i].travel_time * 2.0f + blobs[i].hold_time)) {
                // Moving Up
                float current_phase_time = blobs[i].time_pos - (blobs[i].travel_time + blobs[i].hold_time);
                float linear_t = current_phase_time / blobs[i].travel_time;
                progress = (1.0f - cosf(linear_t * 3.14159f)) / 2.0f;
                target_y = bottom_y - progress * (bottom_y - top_y);
            } 
            else {
                // Stuck completely hidden past the top edge
                target_y = top_y;
            }

            /* Follow the viscosity-scaled trajectory directly. The timeline itself
               is slowed continuously, so the effect remains visible indefinitely. */
            blobs[i].y = target_y;
        }

        refresh();
        usleep(66000); 
    }

    endwin();
    return 0;
}
