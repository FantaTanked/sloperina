/* nativepreview's stand-in for gpuui.c: the PC has no NVN, so panels are drawn the CPU way. */
#include "../ootgz/gpuui.h"

bool gpuui_ready(void) { return false; }
void gpuui_rect(float x, float y, float w, float h, const gpu_colour_t corners[4]) { (void)x, (void)y, (void)w, (void)h, (void)corners; }
void gpuui_fill(float x, float y, float w, float h, gpu_colour_t colour) { (void)x, (void)y, (void)w, (void)h, (void)colour; }
void gpuui_rect_turned(float cx, float cy, float w, float h, float angle, gpu_colour_t colour)
{
  (void)cx, (void)cy, (void)w, (void)h, (void)angle, (void)colour;
}
float gpuui_glyph(gf_face_t face, int em, uint32_t cp, float x, float baseline, gpu_colour_t colour)
{
  (void)face, (void)em, (void)cp, (void)x, (void)baseline, (void)colour;
  return 0;
}
void gpuui_shape(uint32_t id, int w, int h, gpu_paint_t paint, const void *arg, float x, float y, gpu_colour_t colour)
{
  (void)id, (void)w, (void)h, (void)paint, (void)arg, (void)x, (void)y, (void)colour;
}
const char *T(const char *english) { return english; }
void gpuui_images_publish(int batch, int first, uint8_t *mem, size_t size, const gpu_image_t *images, int n)
{
  (void)batch, (void)first, (void)mem, (void)size, (void)images, (void)n;
}
bool gpuui_image_ready(int i) { return (void)i, false; }
void gpuui_image(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t colour)
{
  (void)i, (void)x, (void)y, (void)w, (void)h, (void)crop, (void)colour;
}
int lang_current(void) { return 0; }
bool gpuui_atlas_resetting(void) { return false; }
void gpuui_image_mapped(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t black,
                        gpu_colour_t white, float alpha)
{
  (void)i, (void)x, (void)y, (void)w, (void)h, (void)crop, (void)black, (void)white, (void)alpha;
}
void gpuui_icons_publish(uint8_t *mem, size_t size) { (void)mem, (void)size; }
void gpuui_icon_filled(int slot) { (void)slot; }
bool gpuui_icon_ready(int slot) { return (void)slot, false; }
void gpuui_icon(int slot, float x, float y, float w, float h, const float crop[4], gpu_colour_t colour)
{
  (void)slot, (void)x, (void)y, (void)w, (void)h, (void)crop, (void)colour;
}
void gpuui_scale(float s, float ox, float oy) { (void)s, (void)ox, (void)oy; }
void gpuui_line(float x0, float y0, float x1, float y1, float width, gpu_colour_t colour)
{
  (void)x0, (void)y0, (void)x1, (void)y1, (void)width, (void)colour;
}
