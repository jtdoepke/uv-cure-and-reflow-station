// touch_calibrate — derive a CYD's touch cfg.x_min/x_max/y_min/y_max on the real unit.
//
// Build/flash with `make touch-calib` (env touch_calib_cyd35) or
//   pio run -e touch_calib_cyd35 -t upload --upload-port /dev/ttyUSBn
// Tap each of the 15 circles and hold until it turns green; the values to paste into the board's
// LGFX header are printed on serial, prefixed CALIB.
//
// Why not LovyanGFX's built-in calibrateTouch()? It samples the four TRUE corners — LGFXBase.cpp
// draws its targets at width()-1 / height()-1, half of each behind the bezel — so taps land
// systematically inward and the derived range comes out compressed. Four points also leave no
// redundancy and no way to know how good the fit is. This samples a grid you can hit accurately,
// fits by least squares, and reports the residual IN PIXELS so the number can be judged against
// the design guide's touch floor.
//
// The model matches what the LGFX header stores. Panel_Device::touchCalibrate() packs cfg into
// {x_min,y_min},{x_min,y_max},{x_max,y_min},{x_max,y_max} and setCalibrate() fits those to screen
// corners (0,0),(0,h-1),(w-1,0),(w-1,h-1) — so x_min is raw X at screen x=0 and x_max raw X at
// x=w-1. This fits raw = m*screen + c over the grid and evaluates at exactly those endpoints.
//
// Colour literals here are 0xRRGGBB: LovyanGFX's uint32_t overload is RGB888, so an RGB565
// constant like 0x07E0 ("green") silently paints blue.
#include <Arduino.h>

#include "cyd_board.h"

static LGFX gfx;

// A grid inset from the edges — interior points are where a finger can actually land on the
// intended spot. The fit extrapolates to the edges; it does not need samples there.
static const int XS[] = {30, 160, 290};
static const int YS[] = {30, 142, 240, 366, 450};
static const int NX = sizeof(XS) / sizeof(XS[0]);
static const int NY = sizeof(YS) / sizeof(YS[0]);
static const int NP = NX * NY;

struct Sample {
  int sx, sy;
  float rx, ry;
};
static Sample g_samples[NP];

static void cross(int x, int y, uint32_t col) {
  gfx.drawLine(x - 12, y, x + 12, y, col);
  gfx.drawLine(x, y - 12, x, y + 12, col);
  gfx.drawCircle(x, y, 9, col);
}

// Average raw readings while held. Returns false if the tap never settled.
static bool sampleAt(float &rx, float &ry) {
  lgfx::touch_point_t tp;
  while (gfx.getTouchRaw(&tp, 1) && tp.size) {
    delay(10); // ignore a finger still down from the previous point
  }
  uint32_t t0 = millis();
  while (!(gfx.getTouchRaw(&tp, 1) && tp.size)) {
    if (millis() - t0 > 30000) {
      return false;
    }
    delay(8);
  }
  delay(40); // let the contact settle before averaging — the first ms are the noisiest
  double ax = 0, ay = 0;
  int n = 0;
  uint32_t t1 = millis();
  while (millis() - t1 < 260) {
    if (gfx.getTouchRaw(&tp, 1) && tp.size) {
      ax += tp.x;
      ay += tp.y;
      n++;
    }
    delay(6);
  }
  if (n < 8) {
    return false;
  }
  rx = static_cast<float>(ax / n);
  ry = static_cast<float>(ay / n);
  while (gfx.getTouchRaw(&tp, 1) && tp.size) {
    delay(10);
  }
  return true;
}

// Least squares: raw = m*screen + c
static void fit(bool axis_x, float &m, float &c) {
  double ss = 0, sr = 0, sss = 0, ssr = 0;
  for (const auto &p : g_samples) {
    double s = axis_x ? p.sx : p.sy;
    double r = axis_x ? p.rx : p.ry;
    ss += s;
    sr += r;
    sss += s * s;
    ssr += s * r;
  }
  const double n = NP;
  m = static_cast<float>((n * ssr - ss * sr) / (n * sss - ss * ss));
  c = static_cast<float>((sr - m * ss) / n);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  gfx.init();
  gfx.setRotation(kRotation);
  gfx.setBrightness(255);
  gfx.fillScreen(0x000000u);
  gfx.setTextSize(2);
  gfx.setTextColor(0xFFFFFFu, 0x000000u);
  gfx.setCursor(6, gfx.height() / 2 - 60);
  gfx.println(" Touch calibration");
  gfx.println("");
  gfx.println(" Tap each circle");
  gfx.println(" and HOLD until it");
  gfx.println(" turns green.");
  gfx.println("");
  gfx.println(" Starting in 4s...");
  delay(4000);

  int i = 0;
  for (int yi = 0; yi < NY; yi++) {
    for (int xi = 0; xi < NX; xi++, i++) {
      gfx.fillScreen(0x000000u);
      gfx.setCursor(6, 6);
      gfx.printf(" %d/%d", i + 1, NP);
      cross(XS[xi], YS[yi], 0xFFFFFFu);
      float rx = 0, ry = 0;
      if (!sampleAt(rx, ry)) {
        Serial.println("TIMEOUT - no touch; aborting");
        return;
      }
      g_samples[i] = {XS[xi], YS[yi], rx, ry};
      cross(XS[xi], YS[yi], 0x00FF00u);
      Serial.printf("  pt %2d screen=(%3d,%3d) raw=(%7.1f,%7.1f)\n", i + 1, XS[xi], YS[yi], rx, ry);
      delay(120);
    }
  }

  float mx = 0, cx = 0, my = 0, cy = 0;
  fit(true, mx, cx);
  fit(false, my, cy);
  const int x_min = lroundf(cx);
  const int x_max = lroundf(mx * (panel::kNativeW - 1) + cx);
  const int y_min = lroundf(cy);
  const int y_max = lroundf(my * (panel::kNativeH - 1) + cy);

  // Residual in PIXELS — the only unit in which "is this good enough?" is answerable. Compare
  // against theme::TOUCH_MIN (the design guide's 10 mm floor).
  float max_ex = 0, max_ey = 0, sum_ex = 0, sum_ey = 0;
  for (const auto &p : g_samples) {
    const float ex = fabsf(((p.rx - cx) / mx) - p.sx);
    const float ey = fabsf(((p.ry - cy) / my) - p.sy);
    max_ex = fmaxf(max_ex, ex);
    max_ey = fmaxf(max_ey, ey);
    sum_ex += ex;
    sum_ey += ey;
  }

  Serial.println("\n=== paste into this board's LGFX header ===");
  Serial.printf("CALIB cfg.x_min = %d;\n", x_min);
  Serial.printf("CALIB cfg.x_max = %d;\n", x_max);
  Serial.printf("CALIB cfg.y_min = %d;\n", y_min);
  Serial.printf("CALIB cfg.y_max = %d;\n", y_max);
  Serial.printf("  residual px: x mean=%.1f max=%.1f | y mean=%.1f max=%.1f\n", sum_ex / NP, max_ex,
                sum_ey / NP, max_ey);
  Serial.printf("  spans: x=%d y=%d  (a span near zero means the axes are transposed -> fix\n"
                "         cfg.offset_rotation, not the min/max)\n",
                abs(x_max - x_min), abs(y_max - y_min));
  Serial.println("  DONE");

  gfx.fillScreen(0x000000u);
  gfx.setCursor(6, 10);
  gfx.println(" Done - see serial.");
  gfx.println(" Draw to verify:");
  gfx.println(" the dot should sit");
  gfx.println(" under your finger.");
}

void loop() {
  // Verification: this uses the header's CURRENT values, not the fit just printed — so the dot
  // only tracks correctly once the numbers above are pasted in and this is reflashed.
  int32_t x = 0, y = 0;
  if (gfx.getTouch(&x, &y)) {
    gfx.fillCircle(x, y, 3, 0x00FF00u);
  }
  delay(8);
}
