/*
 * PicoDrive
 * (C) notaz, 2013
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <math.h>
#include <SDL/SDL_ttf.h>

#include "../libpicofe/input.h"
#include "../libpicofe/plat.h"
#include "../libpicofe/plat_sdl.h"
#include "../libpicofe/in_sdl.h"
#include "../libpicofe/gl.h"
#include "emu.h"
#include "configfile_fk.h"
#include "menu_pico.h"
#include "input_pico.h"
#include "plat_sdl.h"
#include "version.h"

#include <pico/pico_int.h>

#define RES_HW_SCREEN_HORIZONTAL  240
#define RES_HW_SCREEN_VERTICAL    240

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ABS(x) (((x) < 0) ? (-x) : (x))



#define AVERAGE(z, x) ((((z) & 0xF7DEF7DE) >> 1) + (((x) & 0xF7DEF7DE) >> 1))
#define AVERAGEHI(AB) ((((AB) & 0xF7DE0000) >> 1) + (((AB) & 0xF7DE) << 15))
#define AVERAGELO(CD) ((((CD) & 0xF7DE) >> 1) + (((CD) & 0xF7DE0000) >> 17))

// Support math
#define Half(A) (((A) >> 1) & 0x7BEF)
#define Quarter(A) (((A) >> 2) & 0x39E7)
// Error correction expressions to piece back the lower bits together
#define RestHalf(A) ((A) & 0x0821)
#define RestQuarter(A) ((A) & 0x1863)

// Error correction expressions for quarters of pixels
#define Corr1_3(A, B)     Quarter(RestQuarter(A) + (RestHalf(B) << 1) + RestQuarter(B))
#define Corr3_1(A, B)     Quarter((RestHalf(A) << 1) + RestQuarter(A) + RestQuarter(B))

// Error correction expressions for halves
#define Corr1_1(A, B)     ((A) & (B) & 0x0821)

// Quarters
#define Weight1_3(A, B)   (Quarter(A) + Half(B) + Quarter(B) + Corr1_3(A, B))
#define Weight3_1(A, B)   (Half(A) + Quarter(A) + Quarter(B) + Corr3_1(A, B))

// Halves
#define Weight1_1(A, B)   (Half(A) + Half(B) + Corr1_1(A, B))

static void *shadow_fb;
static struct area { int w, h; } area;

static struct in_pdata in_sdl_platform_data = {
	.defbinds = in_sdl_defbinds,
	.key_map = in_sdl_key_map,
	.joy_map = in_sdl_joy_map,
};

static struct in_pdata in_sdl_platform_data_SMS = {
  .defbinds = in_sdl_defbinds_SMS,
  .key_map = in_sdl_key_map,
  .joy_map = in_sdl_joy_map,
};

/* YUV stuff */
static int yuv_ry[32], yuv_gy[32], yuv_by[32];
static unsigned char yuv_u[32 * 2], yuv_v[32 * 2];
static unsigned char yuv_y[256];
static struct uyvy { uint32_t y:8; uint32_t vyu:24; } yuv_uyvy[65536];

SDL_Surface * hw_screen = NULL;
SDL_Surface * virtual_hw_screen = NULL;
static SDL_Surface * sms_game_screen = NULL;
static SDL_Surface * gg_game_screen = NULL;

void clear_screen(SDL_Surface *surface, uint16_t color)
{
  if(surface){
    uint16_t *dest_ptr = (uint16_t *)surface->pixels;
    uint32_t x, y;

    for(y = 0; y < surface->h; y++)
    {
      for(x = 0; x < surface->w; x++, dest_ptr++)
      {
        *dest_ptr = color;
      }
    }
  }
}

void bgr_to_uyvy_init(void)
{
  int i, v;

  /* init yuv converter:
    y0 = (int)((0.299f * r0) + (0.587f * g0) + (0.114f * b0));
    y1 = (int)((0.299f * r1) + (0.587f * g1) + (0.114f * b1));
    u = (int)(8 * 0.565f * (b0 - y0)) + 128;
    v = (int)(8 * 0.713f * (r0 - y0)) + 128;
  */
  for (i = 0; i < 32; i++) {
    yuv_ry[i] = (int)(0.299f * i * 65536.0f + 0.5f);
    yuv_gy[i] = (int)(0.587f * i * 65536.0f + 0.5f);
    yuv_by[i] = (int)(0.114f * i * 65536.0f + 0.5f);
  }
  for (i = -32; i < 32; i++) {
    v = (int)(8 * 0.565f * i) + 128;
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    yuv_u[i + 32] = v;
    v = (int)(8 * 0.713f * i) + 128;
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    yuv_v[i + 32] = v;
  }
  // valid Y range seems to be 16..235
  for (i = 0; i < 256; i++) {
    yuv_y[i] = 16 + 219 * i / 32;
  }
  // everything combined into one large array for speed
  for (i = 0; i < 65536; i++) {
     int r = (i >> 11) & 0x1f, g = (i >> 6) & 0x1f, b = (i >> 0) & 0x1f;
     int y = (yuv_ry[r] + yuv_gy[g] + yuv_by[b]) >> 16;
     yuv_uyvy[i].y = yuv_y[y];
#if CPU_IS_LE
     yuv_uyvy[i].vyu = (yuv_v[r-y + 32] << 16) | (yuv_y[y] << 8) | yuv_u[b-y + 32];
#else
     yuv_uyvy[i].vyu = (yuv_v[b-y + 32] << 16) | (yuv_y[y] << 8) | yuv_u[r-y + 32];
#endif
  }
}

void rgb565_to_uyvy(void *d, const void *s, int w, int h, int pitch, int x2)
{
  uint32_t *dst = d;
  const uint16_t *src = s;
  int i;

  if (x2) while (h--) {
    for (i = w; i > 0; src += 4, dst += 4, i -= 4)
    {
      struct uyvy *uyvy0 = yuv_uyvy + src[0], *uyvy1 = yuv_uyvy + src[1];
      struct uyvy *uyvy2 = yuv_uyvy + src[2], *uyvy3 = yuv_uyvy + src[3];
#if CPU_IS_LE
      dst[0] = (uyvy0->y << 24) | uyvy0->vyu;
      dst[1] = (uyvy1->y << 24) | uyvy1->vyu;
      dst[2] = (uyvy2->y << 24) | uyvy2->vyu;
      dst[3] = (uyvy3->y << 24) | uyvy3->vyu;
#else
      dst[0] = uyvy0->y | (uyvy0->vyu << 8);
      dst[1] = uyvy1->y | (uyvy1->vyu << 8);
      dst[2] = uyvy2->y | (uyvy2->vyu << 8);
      dst[3] = uyvy3->y | (uyvy3->vyu << 8);
#endif
      }
    src += pitch - w;
  } else while (h--) {
    for (i = w; i > 0; src += 4, dst += 2, i -= 4)
    {
      struct uyvy *uyvy0 = yuv_uyvy + src[0], *uyvy1 = yuv_uyvy + src[1];
      struct uyvy *uyvy2 = yuv_uyvy + src[2], *uyvy3 = yuv_uyvy + src[3];
#if CPU_IS_LE
      dst[0] = (uyvy1->y << 24) | uyvy0->vyu;
      dst[1] = (uyvy3->y << 24) | uyvy2->vyu;
#else
      dst[0] = uyvy1->y | (uyvy0->vyu << 8);
      dst[1] = uyvy3->y | (uyvy2->vyu << 8);
#endif
    }
    src += pitch - w;
  }
}








// Nearest neighboor
void flip_NN(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w2=new_w;
  int h2=new_h;
  int x_ratio = (int)((virtual_screen->w<<16)/w2) +1;
  int y_ratio = (int)((virtual_screen->h<<16)/h2) +1;
  //int x_ratio = (int)((w1<<16)/w2) ;
  //int y_ratio = (int)((h1<<16)/h2) ;
  //printf("virtual_screen->w=%d, virtual_screen->h=%d\n", virtual_screen->w, virtual_screen->h);
  int x2, y2 ;
  for (int i=0;i<h2;i++) {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    //printf("\n\ny=%d\n", i);
    for (int j=0;j<w2;j++) {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      //printf("x=%d, ",j);
      x2 = ((j*x_ratio)>>16) ;
      y2 = ((i*y_ratio)>>16) ;

      //printf("y=%d, x=%d, y2=%d, x2=%d, (y2*virtual_screen->w)+x2=%d\n", i, j, y2, x2, (y2*virtual_screen->w)+x2);
      *(uint16_t*)(hardware_screen->pixels+(i* ((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2 ) +j)*sizeof(uint16_t)) =
      *(uint16_t*)(virtual_screen->pixels + ((y2*virtual_screen->w)+x2) *sizeof(uint16_t)) ;
    }
  }
}

// Nearest neighboor with possible out of screen coordinates (for cropping)
void flip_NN_AllowOutOfScreen(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w2=new_w;
  int h2=new_h;
  int x_ratio = (int)((virtual_screen->w<<16)/w2) +1;
  int y_ratio = (int)((virtual_screen->h<<16)/h2) +1;
  //int x_ratio = (int)((w1<<16)/w2) ;
  //int y_ratio = (int)((h1<<16)/h2) ;
  //printf("virtual_screen->w=%d, virtual_screen->h=%d\n", virtual_screen->w, virtual_screen->h);
  int x2, y2 ;

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }

  for (int i=0;i<h2;i++) {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    //printf("\n\ny=%d\n", i);
    for (int j=0;j<w2;j++) {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      //printf("x=%d, ",j);
      x2 = ((j*x_ratio)>>16) ;
      y2 = ((i*y_ratio)>>16) ;

      //printf("y=%d, x=%d, y2=%d, x2=%d, (y2*virtual_screen->w)+x2=%d\n", i, j, y2, x2, (y2*virtual_screen->w)+x2);
      *(uint16_t*)(hardware_screen->pixels+(i* ((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2 ) +j)*sizeof(uint16_t)) =
      *(uint16_t*)(virtual_screen->pixels + ((y2*virtual_screen->w)+x2 + x_padding) *sizeof(uint16_t)) ;
    }
  }
}

/// Nearest neighboor optimized with possible out of screen coordinates (for cropping)
void flip_NNOptimized_AllowOutOfScreen(SDL_Surface *src_surface, SDL_Surface *dst_surface, int new_w, int new_h){
  int x_ratio = (int)((src_surface->w<<16)/new_w);
  int y_ratio = (int)((src_surface->h<<16)/new_h);
  int x2, y2;
  /*printf("src_surface dimensions: %dx%d, new dimensions: %dx%d\n", 
          src_surface->w, src_surface->h, new_w, new_h);*/

  /// --- Compute padding for centering when out of bounds ---
  int y_padding_dst = MAX((RES_HW_SCREEN_VERTICAL-new_h)/2, 0);
  int x_padding_dst = MAX((RES_HW_SCREEN_HORIZONTAL-new_w)/2, 0);
  /*printf("padding_dst_x: %d, padding_dst_y: %d\n", x_padding_dst, y_padding_dst);*/

  /// --- Compute offset coordinates in src surface
  int x_offset_src = (new_w>RES_HW_SCREEN_HORIZONTAL) ? (new_w-RES_HW_SCREEN_HORIZONTAL)/2 + 1 : 0;
  int x_offset_src_ratio = x_offset_src*src_surface->w/new_w;
  /*printf("x_offset_src: %d,\n", x_offset_src);*/

  for (int i=0;i<new_h;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }

    uint16_t* t = (uint16_t*)(dst_surface->pixels + 
                              ( (i+y_padding_dst)* dst_surface->w + x_padding_dst )*sizeof(uint16_t));
    y2 = ((i*y_ratio)>>16);
    uint16_t* p = (uint16_t*)(src_surface->pixels + (y2*src_surface->w + x_offset_src_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<new_w;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      x2 = (rat>>16);
      *t++ = p[x2];
      rat += x_ratio;
      //printf("y=%d, x=%d, y2=%d, x2=%d, (y2*src_surface->w)+x2=%d\n", i, j, y2, x2, (y2*src_surface->w)+x2);
    }
  }
}


/// Nearest neighboor with 2D bilinear and interp by the number of pixel diff, not 2
void flip_NNOptimized_MissingPixelsBilinear(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  //int x_ratio = (int)((w1<<16)/w2) +1;
  //int y_ratio = (int)((h1<<16)/h2) +1;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;
  /*int cnt_yes_x_yes_y, cnt_yes_x_no_y, cnt_no_x_yes_y, cnt_no_x_no_y;
  cnt_yes_x_yes_y= cnt_yes_x_no_y= cnt_no_x_yes_y= cnt_no_x_no_y = 0;*/
  for (int i=0;i<h2;i++)
  {
    uint16_t* t = (uint16_t*)(hardware_screen->pixels+((i+y_padding)*w2)*sizeof(uint16_t));
    y1 = ((i*y_ratio)>>16);
    int px_diff_next_y = MAX( (((i+1)*y_ratio)>>16) - y1, 1);
    //printf("px_diff_next_y:%d\n", px_diff_next_y);
    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      // ------ current x value ------
      x1 = (rat>>16);
      int px_diff_next_x = MAX( ((rat+x_ratio)>>16) - x1, 1);

      // ------ optimized bilinear (to put in function) -------
      uint16_t * cur_p;
      int cur_y_offset;
      uint32_t red_comp = 0;
      uint32_t green_comp = 0;
      uint32_t blue_comp = 0;
      for(int cur_px_diff_y=0; cur_px_diff_y<px_diff_next_y; cur_px_diff_y++){
        cur_y_offset = (y1+cur_px_diff_y<h1)?(w1*cur_px_diff_y):0;
        for(int cur_px_diff_x=0; cur_px_diff_x<px_diff_next_x; cur_px_diff_x++){
          cur_p = (x1+cur_px_diff_x<w1)?(p+x1+cur_px_diff_x+cur_y_offset):(p+x1+cur_y_offset);
          red_comp += (*cur_p)&0xF800;
          green_comp += (*cur_p)&0x07E0;
          blue_comp += (*cur_p)&0x001F;
        }
      }
      red_comp = (red_comp / (px_diff_next_x*px_diff_next_y) )&0xF800;
      green_comp = (green_comp / (px_diff_next_x*px_diff_next_y) )&0x07E0;
      blue_comp = (blue_comp / (px_diff_next_x*px_diff_next_y) )&0x001F;
      *t++ = red_comp+green_comp+blue_comp;

      // ------ next pixel ------
      rat += x_ratio;
    }
  }
}


/// Nearest neighbor with 2D bilinear and interpolation with left and right pixels, pseudo gaussian weighting
void flip_NNOptimized_LeftAndRightBilinear(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  //int x_ratio = (int)((w1<<16)/w2) +1;
  //int y_ratio = (int)((h1<<16)/h2) +1;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;

  uint16_t green_mask = 0x07E0;

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// --- Interp params ---
  int px_diff_prev_x = 0;
  int px_diff_next_x = 0;
  uint32_t ponderation_factor;
  uint16_t * cur_p;
  uint16_t * cur_p_left;
  uint16_t * cur_p_right;
  uint32_t red_comp, green_comp, blue_comp;
  //int cnt_interp = 0; int cnt_no_interp = 0;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);

  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));
    y1 = ((i*y_ratio)>>16);
    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1 + x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      // ------ current x value ------
      x1 = (rat>>16);
      px_diff_next_x = ((rat+x_ratio)>>16) - x1;

      // ------ adapted bilinear with 3x3 gaussian blur -------
      cur_p = p+x1;
      if(px_diff_prev_x > 1 || px_diff_next_x > 1){
        red_comp=((*cur_p)&0xF800) << 1;
        green_comp=((*cur_p)&0x07E0) << 1;
        blue_comp=((*cur_p)&0x001F) << 1;
        ponderation_factor = 2;

        // ---- Interpolate current and left ----
        if(px_diff_prev_x > 1 && x1>0){
          cur_p_left = p+x1-1;

          red_comp += ((*cur_p_left)&0xF800);
          green_comp += ((*cur_p_left)&0x07E0);
          blue_comp += ((*cur_p_left)&0x001F);
          ponderation_factor++;
        }

        // ---- Interpolate current and right ----
        if(px_diff_next_x > 1 && x1+1<w1){
          cur_p_right = p+x1+1;

          red_comp += ((*cur_p_right)&0xF800);
          green_comp += ((*cur_p_right)&0x07E0);
          blue_comp += ((*cur_p_right)&0x001F);
          ponderation_factor++;
        }

        /// --- Compute new px value ---
        if(ponderation_factor==4){
          red_comp = (red_comp >> 2)&0xF800;
          green_comp = (green_comp >> 2)&green_mask;
          blue_comp = (blue_comp >> 2)&0x001F;
        }
        else if(ponderation_factor==2){
          red_comp = (red_comp >> 1)&0xF800;
          green_comp = (green_comp >> 1)&green_mask;
          blue_comp = (blue_comp >> 1)&0x001F;
        }
        else{
          red_comp = (red_comp / ponderation_factor )&0xF800;
          green_comp = (green_comp / ponderation_factor )&green_mask;
          blue_comp = (blue_comp / ponderation_factor )&0x001F;
        }

        /// --- write pixel ---
        *t++ = red_comp+green_comp+blue_comp;
      }
      else{
        /// --- copy pixel ---
        *t++ = (*cur_p);
      }

      /// save number of pixels to interpolate
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }
  }
  //printf("cnt_interp = %d, int cnt_no_interp = %d\n", cnt_interp, cnt_no_interp);
}

/// Nearest neighbor with 2D bilinear and interpolation with left, right, up and down pixels, pseudo gaussian weighting
void flip_NNOptimized_LeftRightUpDownBilinear(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  //int x_ratio = (int)((w1<<16)/w2) +1;
  //int y_ratio = (int)((h1<<16)/h2) +1;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;

  uint16_t green_mask = 0x07E0;

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// --- Interp params ---
  int px_diff_prev_x = 0;
  int px_diff_next_x = 0;
  int px_diff_prev_y = 0;
  int px_diff_next_y = 0;
  uint32_t ponderation_factor;
  uint16_t * cur_p;
  uint16_t * cur_p_left;
  uint16_t * cur_p_right;
  uint16_t * cur_p_up;
  uint16_t * cur_p_down;
  uint32_t red_comp, green_comp, blue_comp;
  //int cnt_interp = 0; int cnt_no_interp = 0;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);

  ///Debug

  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));
    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    px_diff_next_y = MAX( (((i+1)*y_ratio)>>16) - y1, 1);
    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1+x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      // ------ current x value ------
      x1 = (rat>>16);
      px_diff_next_x = ((rat+x_ratio)>>16) - x1;

      // ------ adapted bilinear with 3x3 gaussian blur -------
      cur_p = p+x1;
      if(px_diff_prev_x > 1 || px_diff_next_x > 1 || px_diff_prev_y > 1 || px_diff_next_y > 1){
        red_comp=((*cur_p)&0xF800) << 1;
        green_comp=((*cur_p)&0x07E0) << 1;
        blue_comp=((*cur_p)&0x001F) << 1;
        ponderation_factor = 2;

        // ---- Interpolate current and left ----
        if(px_diff_prev_x > 1 && x1>0){
          cur_p_left = p+x1-1;

          red_comp += ((*cur_p_left)&0xF800);
          green_comp += ((*cur_p_left)&0x07E0);
          blue_comp += ((*cur_p_left)&0x001F);
          ponderation_factor++;
        }

        // ---- Interpolate current and right ----
        if(px_diff_next_x > 1 && x1+1<w1){
          cur_p_right = p+x1+1;

          red_comp += ((*cur_p_right)&0xF800);
          green_comp += ((*cur_p_right)&0x07E0);
          blue_comp += ((*cur_p_right)&0x001F);
          ponderation_factor++;
        }

        // ---- Interpolate current and up ----
        if(px_diff_prev_y > 1 && y1 > 0){
          cur_p_up = p+x1-w1;

          red_comp += ((*cur_p_up)&0xF800);
          green_comp += ((*cur_p_up)&0x07E0);
          blue_comp += ((*cur_p_up)&0x001F);
          ponderation_factor++;
        }

        // ---- Interpolate current and down ----
        if(px_diff_next_y > 1 && y1 + 1 < h1){
          cur_p_down = p+x1+w1;

          red_comp += ((*cur_p_down)&0xF800);
          green_comp += ((*cur_p_down)&0x07E0);
          blue_comp += ((*cur_p_down)&0x001F);
          ponderation_factor++;
        }

        /// --- Compute new px value ---
        if(ponderation_factor==4){
          red_comp = (red_comp >> 2)&0xF800;
          green_comp = (green_comp >> 2)&green_mask;
          blue_comp = (blue_comp >> 2)&0x001F;
        }
        else if(ponderation_factor==2){
          red_comp = (red_comp >> 1)&0xF800;
          green_comp = (green_comp >> 1)&green_mask;
          blue_comp = (blue_comp >> 1)&0x001F;
        }
        else{
          red_comp = (red_comp / ponderation_factor )&0xF800;
          green_comp = (green_comp / ponderation_factor )&green_mask;
          blue_comp = (blue_comp / ponderation_factor )&0x001F;
        }

        /// --- write pixel ---
        *t++ = red_comp+green_comp+blue_comp;
      }
      else{
        /// --- copy pixel ---
        *t++ = (*cur_p);
      }

      /// save number of pixels to interpolate
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }
    px_diff_prev_y = px_diff_next_y;
  }
  //printf("cnt_interp = %d, int cnt_no_interp = %d\n", cnt_interp, cnt_no_interp);
}



/// Nearest neighbor with 2D bilinear and interpolation with left, right, up and down pixels, pseudo gaussian weighting
void flip_NNOptimized_LeftRightUpDownBilinear_Optimized4(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;

  uint16_t green_mask = 0x07E0;

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// --- Interp params ---
  int px_diff_prev_x = 0;
  int px_diff_next_x = 0;
  int px_diff_prev_y = 0;
  int px_diff_next_y = 0;
  uint32_t ponderation_factor;
  uint8_t left_px_missing, right_px_missing, up_px_missing, down_px_missing;
  int supposed_pond_factor;

  uint16_t * cur_p;
  uint16_t * cur_p_left;
  uint16_t * cur_p_right;
  uint16_t * cur_p_up;
  uint16_t * cur_p_down;
  uint32_t red_comp, green_comp, blue_comp;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);

  ///Debug
  /*int occurence_pond[7];
  memset(occurence_pond, 0, 7*sizeof(int));*/

  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));
    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    px_diff_next_y = MAX( (((i+1)*y_ratio)>>16) - y1, 1);
    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1+x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      // ------ current x value ------
      x1 = (rat>>16);
      px_diff_next_x = ((rat+x_ratio)>>16) - x1;

      // ------ adapted bilinear with 3x3 gaussian blur -------
      cur_p = p+x1;
      if(px_diff_prev_x > 1 || px_diff_next_x > 1 || px_diff_prev_y > 1 || px_diff_next_y > 1){
        red_comp=((*cur_p)&0xF800) << 1;
        green_comp=((*cur_p)&0x07E0) << 1;
        blue_comp=((*cur_p)&0x001F) << 1;
        ponderation_factor = 2;
        left_px_missing = (px_diff_prev_x > 1 && x1>0);
        right_px_missing = (px_diff_next_x > 1 && x1+1<w1);
        up_px_missing = (px_diff_prev_y > 1 && y1 > 0);
        down_px_missing = (px_diff_next_y > 1 && y1 + 1 < h1);
        supposed_pond_factor = 2 + left_px_missing + right_px_missing +
                                       up_px_missing + down_px_missing;

        // ---- Interpolate current and up ----
        if(up_px_missing){
          cur_p_up = p+x1-w1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_up)&0xF800) << 1;
            green_comp += ((*cur_p_up)&0x07E0) << 1;
            blue_comp += ((*cur_p_up)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor==4 ||
                  (supposed_pond_factor==5 && !down_px_missing )){
            red_comp += ((*cur_p_up)&0xF800);
            green_comp += ((*cur_p_up)&0x07E0);
            blue_comp += ((*cur_p_up)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and left ----
        if(left_px_missing){
          cur_p_left = p+x1-1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_left)&0xF800) << 1;
            green_comp += ((*cur_p_left)&0x07E0) << 1;
            blue_comp += ((*cur_p_left)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor==4 ||
                  (supposed_pond_factor==5 && !right_px_missing )){
            red_comp += ((*cur_p_left)&0xF800);
            green_comp += ((*cur_p_left)&0x07E0);
            blue_comp += ((*cur_p_left)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and down ----
        if(down_px_missing){
          cur_p_down = p+x1+w1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_down)&0xF800) << 1;
            green_comp += ((*cur_p_down)&0x07E0) << 1;
            blue_comp += ((*cur_p_down)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor>=4){
            red_comp += ((*cur_p_down)&0xF800);
            green_comp += ((*cur_p_down)&0x07E0);
            blue_comp += ((*cur_p_down)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and right ----
        if(right_px_missing){
          cur_p_right = p+x1+1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_right)&0xF800) << 1;
            green_comp += ((*cur_p_right)&0x07E0) << 1;
            blue_comp += ((*cur_p_right)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor>=4){
            red_comp += ((*cur_p_right)&0xF800);
            green_comp += ((*cur_p_right)&0x07E0);
            blue_comp += ((*cur_p_right)&0x001F);
            ponderation_factor++;
          }
        }

        /// --- Compute new px value ---
        if(ponderation_factor==4){
          red_comp = (red_comp >> 2)&0xF800;
          green_comp = (green_comp >> 2)&green_mask;
          blue_comp = (blue_comp >> 2)&0x001F;
        }
        else if(ponderation_factor==2){
          red_comp = (red_comp >> 1)&0xF800;
          green_comp = (green_comp >> 1)&green_mask;
          blue_comp = (blue_comp >> 1)&0x001F;
        }
        else{
          red_comp = (red_comp / ponderation_factor )&0xF800;
          green_comp = (green_comp / ponderation_factor )&green_mask;
          blue_comp = (blue_comp / ponderation_factor )&0x001F;
        }

        /// Debug
        //occurence_pond[ponderation_factor] += 1;

        /// --- write pixel ---
        *t++ = red_comp+green_comp+blue_comp;
      }
      else{
        /// --- copy pixel ---
        *t++ = (*cur_p);

        /// Debug
        //occurence_pond[1] += 1;
      }

      /// save number of pixels to interpolate
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }
    px_diff_prev_y = px_diff_next_y;
  }
  /// Debug
  /*printf("pond: [%d, %d, %d, %d, %d, %d]\n", occurence_pond[1], occurence_pond[2], occurence_pond[3],
                                              occurence_pond[4], occurence_pond[5], occurence_pond[6]);*/
}



/// Nearest neighbor with 2D bilinear and interpolation with left, right, up and down pixels, pseudo gaussian weighting
void flip_NNOptimized_LeftRightUpDownBilinear_Optimized8(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  //int x_ratio = (int)((w1<<16)/w2) +1;
  //int y_ratio = (int)((h1<<16)/h2) +1;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;

#ifdef BLACKER_BLACKS
      /// Optimization for blacker blacks (our screen do not handle green value of 1 very well)
      uint16_t green_mask = 0x07C0;
#else
      uint16_t green_mask = 0x07E0;
#endif

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// --- Interp params ---
  int px_diff_prev_x = 0;
  int px_diff_next_x = 0;
  int px_diff_prev_y = 0;
  int px_diff_next_y = 0;
  uint32_t ponderation_factor;
  uint8_t left_px_missing, right_px_missing, up_px_missing, down_px_missing;
  int supposed_pond_factor;

  uint16_t * cur_p;
  uint16_t * cur_p_left;
  uint16_t * cur_p_right;
  uint16_t * cur_p_up;
  uint16_t * cur_p_down;
  uint32_t red_comp, green_comp, blue_comp;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);

  ///Debug
  /*int occurence_pond[9];
  memset(occurence_pond, 0, 9*sizeof(int));*/

  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));
    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    px_diff_next_y = MAX( (((i+1)*y_ratio)>>16) - y1, 1);
    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1+x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }
      // ------ current x value ------
      x1 = (rat>>16);
      px_diff_next_x = ((rat+x_ratio)>>16) - x1;

      // ------ adapted bilinear with 3x3 gaussian blur -------
      cur_p = p+x1;
      if(px_diff_prev_x > 1 || px_diff_next_x > 1 || px_diff_prev_y > 1 || px_diff_next_y > 1){
        red_comp=((*cur_p)&0xF800) << 1;
        green_comp=((*cur_p)&0x07E0) << 1;
        blue_comp=((*cur_p)&0x001F) << 1;
        ponderation_factor = 2;
        left_px_missing = (px_diff_prev_x > 1 && x1>0);
        right_px_missing = (px_diff_next_x > 1 && x1+1<w1);
        up_px_missing = (px_diff_prev_y > 1 && y1 > 0);
        down_px_missing = (px_diff_next_y > 1 && y1 + 1 < h1);
        supposed_pond_factor = 2 + left_px_missing + right_px_missing +
                                       up_px_missing + down_px_missing;

        // ---- Interpolate current and up ----
        if(up_px_missing){
          cur_p_up = p+x1-w1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_up)&0xF800) << 1;
            green_comp += ((*cur_p_up)&0x07E0) << 1;
            blue_comp += ((*cur_p_up)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor == 4 ||
                  (supposed_pond_factor == 5 && !down_px_missing) ||
                  supposed_pond_factor == 6 ){
            red_comp += ((*cur_p_up)&0xF800);
            green_comp += ((*cur_p_up)&0x07E0);
            blue_comp += ((*cur_p_up)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and left ----
        if(left_px_missing){
          cur_p_left = p+x1-1;

          if(supposed_pond_factor==3){
            red_comp += ((*cur_p_left)&0xF800) << 1;
            green_comp += ((*cur_p_left)&0x07E0) << 1;
            blue_comp += ((*cur_p_left)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor == 4 ||
                  (supposed_pond_factor == 5 && !right_px_missing) ||
                  supposed_pond_factor == 6 ){
            red_comp += ((*cur_p_left)&0xF800);
            green_comp += ((*cur_p_left)&0x07E0);
            blue_comp += ((*cur_p_left)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and down ----
        if(down_px_missing){
          cur_p_down = p+x1+w1;

          if(supposed_pond_factor==3 || supposed_pond_factor==6){
            red_comp += ((*cur_p_down)&0xF800) << 1;
            green_comp += ((*cur_p_down)&0x07E0) << 1;
            blue_comp += ((*cur_p_down)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor >= 4 && supposed_pond_factor != 6){
            red_comp += ((*cur_p_down)&0xF800);
            green_comp += ((*cur_p_down)&0x07E0);
            blue_comp += ((*cur_p_down)&0x001F);
            ponderation_factor++;
          }
        }

        // ---- Interpolate current and right ----
        if(right_px_missing){
          cur_p_right = p+x1+1;

          if(supposed_pond_factor==3 || supposed_pond_factor==6){
            red_comp += ((*cur_p_right)&0xF800) << 1;
            green_comp += ((*cur_p_right)&0x07E0) << 1;
            blue_comp += ((*cur_p_right)&0x001F) << 1;
            ponderation_factor+=2;
          }
          else if(supposed_pond_factor >= 4 && supposed_pond_factor != 6){
            red_comp += ((*cur_p_right)&0xF800);
            green_comp += ((*cur_p_right)&0x07E0);
            blue_comp += ((*cur_p_right)&0x001F);
            ponderation_factor++;
          }
        }

        /// --- Compute new px value ---
        if(ponderation_factor==8){
          red_comp = (red_comp >> 3)&0xF800;
          green_comp = (green_comp >> 3)&green_mask;
          blue_comp = (blue_comp >> 3)&0x001F;
        }
        else if(ponderation_factor==4){
          red_comp = (red_comp >> 2)&0xF800;
          green_comp = (green_comp >> 2)&green_mask;
          blue_comp = (blue_comp >> 2)&0x001F;
        }
        else if(ponderation_factor==2){
          red_comp = (red_comp >> 1)&0xF800;
          green_comp = (green_comp >> 1)&green_mask;
          blue_comp = (blue_comp >> 1)&0x001F;
        }
        else{
          red_comp = (red_comp / ponderation_factor )&0xF800;
          green_comp = (green_comp / ponderation_factor )&green_mask;
          blue_comp = (blue_comp / ponderation_factor )&0x001F;
        }

        /// Debug
        //occurence_pond[ponderation_factor] += 1;

        /// --- write pixel ---
        *t++ = red_comp+green_comp+blue_comp;
      }
      else{
        /// --- copy pixel ---
        *t++ = (*cur_p);

        /// Debug
        //occurence_pond[1] += 1;
      }

      /// save number of pixels to interpolate
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }
    px_diff_prev_y = px_diff_next_y;
  }
  /// Debug
  /*printf("pond: [%d, %d, %d, %d, %d, %d, %d, %d]\n", occurence_pond[1], occurence_pond[2], occurence_pond[3],
                                              occurence_pond[4], occurence_pond[5], occurence_pond[6],
                                              occurence_pond[7], occurence_pond[8]);*/
}


/// Nearest neighbor with full 2D uniform bilinear  (interpolation with missing left, right, up and down pixels)
void flip_NNOptimized_FullBilinear_Uniform(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  //int x_ratio = (int)((w1<<16)/w2) +1;
  //int y_ratio = (int)((h1<<16)/h2) +1;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;
  int px_diff_prev_x = 1;
  int px_diff_prev_y = 1;
  //int cnt_interp = 0; int cnt_no_interp = 0;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);

  /// ---- Compute padding for centering when out of bounds ----
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// ---- Copy and interpolate pixels ----
  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }

    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));

    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    int px_diff_next_y = MAX( (((i+1)*y_ratio)>>16) - y1, 1);

    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1 + x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }

      // ------ current and next x value ------
      x1 = (rat>>16);
      int px_diff_next_x = MAX( ((rat+x_ratio)>>16) - x1, 1);

      // ------ bilinear uniformly weighted --------
      uint32_t red_comp=0, green_comp=0, blue_comp=0, ponderation_factor=0;
      uint16_t * cur_p;
      int cur_y_offset;

      //printf("\npx_diff_prev_y=%d, px_diff_prev_x=%d, px_diff_next_y=%d, px_diff_next_x=%d, interp_px=", px_diff_prev_y, px_diff_prev_x, px_diff_next_y, px_diff_next_x);

      for(int cur_px_diff_y=-(px_diff_prev_y-1); cur_px_diff_y<px_diff_next_y; cur_px_diff_y++){
        if(y1 + cur_px_diff_y >= h1 || y1 < -cur_px_diff_y){
          continue;
        }
        cur_y_offset = w1*cur_px_diff_y;
        //printf("cur_diff_y=%d-> ", cur_px_diff_y);

        for(int cur_px_diff_x=-(px_diff_prev_x-1); cur_px_diff_x<px_diff_next_x; cur_px_diff_x++){
          if(x1 + cur_px_diff_x >= w1 || x1 < -cur_px_diff_x){
            continue;
          }
          cur_p = (p+cur_y_offset+x1+cur_px_diff_x);
          //printf("{y=%d,x=%d}, ", y1+cur_px_diff_y, x1+cur_px_diff_x);
          red_comp += ((*cur_p)&0xF800);
          green_comp += ((*cur_p)&0x07E0);
          blue_comp += ((*cur_p)&0x001F);
          ponderation_factor++;
        }
      }
      //printf("\n");

      /// ------ Ponderation -------
      red_comp = (red_comp / ponderation_factor )&0xF800;
      green_comp = (green_comp / ponderation_factor )&0x07E0;
      blue_comp = (blue_comp / ponderation_factor )&0x001F;
      *t++ = red_comp+green_comp+blue_comp;

      /// ------ x Interpolation values -------
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }

    /// ------ y Interpolation values -------
    px_diff_prev_y = px_diff_next_y;
  }
  //printf("cnt_interp = %d, int cnt_no_interp = %d\n", cnt_interp, cnt_no_interp);
}


/// Nearest neighbor with full 2D uniform bilinear  (interpolation with missing left, right, up and down pixels)
void flip_NNOptimized_FullBilinear_GaussianWeighted(SDL_Surface *virtual_screen, SDL_Surface *hardware_screen, int new_w, int new_h){
  int w1=virtual_screen->w;
  int h1=virtual_screen->h;
  int w2=new_w;
  int h2=new_h;
  //printf("virtual_screen->w=%d, virtual_screen->w=%d\n", virtual_screen->w, virtual_screen->h);
  int y_padding = (RES_HW_SCREEN_VERTICAL-new_h)/2;
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int x1, y1;
  int px_diff_prev_x = 1;
  int px_diff_prev_y = 1;
  //int cnt_interp = 0; int cnt_no_interp = 0;

  /// ---- Compute padding for centering when out of bounds ----
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// ---- Interpolation params ----
  uint32_t max_pix_interpolate = 3;
  if(max_pix_interpolate > 3 || max_pix_interpolate<1){
    printf("ERROR cannot interpolate more than 3x3 px in flip_NNOptimized_FullBilinear_GaussianWeighted\n");
    return;
  }

  /// ---- Convolutional mask ----
  int mask_weight_5x5[] = {36, 24, 6,   24, 16, 4,    6, 4, 1};
  int mask_weight_3x3[] = {4, 2,  2, 1};
  int mask_weight_1x1[] = {1};
  int *mask_weight;
  if(max_pix_interpolate==3){
    mask_weight = mask_weight_5x5;
  }
  else if(max_pix_interpolate==2){
    mask_weight = mask_weight_3x3;
  }
  else{
    mask_weight = mask_weight_1x1;
  }

  /// ---- Copy and interpolate pixels ----
  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }

    uint16_t* t = (uint16_t*)(hardware_screen->pixels+( (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2))*sizeof(uint16_t));

    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    int px_diff_next_y = MIN( MAX( (((i+1)*y_ratio)>>16) - y1, 1), max_pix_interpolate);

    uint16_t* p = (uint16_t*)(virtual_screen->pixels + (y1*w1 + x_padding_ratio) *sizeof(uint16_t));
    int rat = 0;
    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }

      // ------ current and next x value ------
      x1 = (rat>>16);
      int px_diff_next_x = MIN( MAX( ((rat+x_ratio)>>16) - x1, 1), max_pix_interpolate); //we interpolate max "max_pix_interpolate" pix in each dim

      // ------ bilinear uniformly weighted --------
      uint32_t red_comp=0, green_comp=0, blue_comp=0;
      int ponderation_factor=0;
      uint16_t * cur_p;
      int cur_y_offset;

      //printf("\npx_diff_prev_y=%d, px_diff_prev_x=%d, px_diff_next_y=%d, px_diff_next_x=%d, interp_px=", px_diff_prev_y, px_diff_prev_x, px_diff_next_y, px_diff_next_x);

      for(int cur_px_diff_y=-(px_diff_prev_y-1); cur_px_diff_y<px_diff_next_y; cur_px_diff_y++){
        if(y1 + cur_px_diff_y >= h1 || y1 < -cur_px_diff_y){
          continue;
        }
        cur_y_offset = w1*cur_px_diff_y;
        //printf("cur_diff_y=%d-> ", cur_px_diff_y);

        for(int cur_px_diff_x=-(px_diff_prev_x-1); cur_px_diff_x<px_diff_next_x; cur_px_diff_x++){
          if(x1 + cur_px_diff_x >= w1 || x1 < -cur_px_diff_x){
            continue;
          }
          cur_p = (p+cur_y_offset+x1+cur_px_diff_x);
          int weight = mask_weight[ABS(cur_px_diff_y)*max_pix_interpolate+ABS(cur_px_diff_x)];

          red_comp += ((*cur_p)&0xF800) * weight;
          green_comp += ((*cur_p)&0x07E0) * weight;
          blue_comp += ((*cur_p)&0x001F) * weight;
          ponderation_factor += weight;
        }
      }
      //printf("\n");

      /// ------ Ponderation -------
      red_comp = (red_comp / ponderation_factor) & 0xF800;
      green_comp = (green_comp / ponderation_factor )&0x07E0;
      blue_comp = (blue_comp / ponderation_factor) & 0x001F;
      *t++ = red_comp+green_comp+blue_comp;

      /// ------ x Interpolation values -------
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }

    /// ------ y Interpolation values -------
    px_diff_prev_y = px_diff_next_y;
  }
  //printf("cnt_interp = %d, int cnt_no_interp = %d\n", cnt_interp, cnt_no_interp);
}



/// Interpolation with left, right pixels, pseudo gaussian weighting for downscaling - operations on 16bits
void flip_Downscale_LeftRightGaussianFilter_Optimized(SDL_Surface *src_surface, SDL_Surface *dst_surface, int new_w, int new_h){
  int w1=src_surface->w;
  int h1=src_surface->h;
  int w2=dst_surface->w;
  int h2=dst_surface->h;
  //printf("src = %dx%d\n", w1, h1);
  int x_ratio = (int)((w1<<16)/w2);
  int y_ratio = (int)((h1<<16)/h2);
  int y_padding = (RES_HW_SCREEN_VERTICAL-h2)/2;
  int x1, y1;
  uint16_t *src_screen = (uint16_t *)src_surface->pixels;
  uint16_t *dst_screen = (uint16_t *)dst_surface->pixels;

  /// --- Compute padding for centering when out of bounds ---
  int x_padding = 0;
  if(w2>RES_HW_SCREEN_HORIZONTAL){
    x_padding = (w2-RES_HW_SCREEN_HORIZONTAL)/2 + 1;
  }
  int x_padding_ratio = x_padding*w1/w2;

  /// --- Interp params ---
  int px_diff_prev_x = 0;
  int px_diff_next_x = 0;
  uint8_t left_px_missing, right_px_missing;

  uint16_t * cur_p;
  uint16_t * cur_p_left;
  uint16_t * cur_p_right;


  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(dst_screen +
      (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2) );
    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    uint16_t* p = (uint16_t*)(src_screen + (y1*w1+x_padding_ratio) );
    int rat = 0;

    for (int j=0;j<w2;j++)
    {
      if(j>=RES_HW_SCREEN_HORIZONTAL){
        continue;
      }

      // ------ current x value ------
      x1 = (rat>>16);
      px_diff_next_x = ((rat+x_ratio)>>16) - x1;

      //printf("x1=%d, px_diff_prev_x=%d, px_diff_next_x=%d\n", x1, px_diff_prev_x, px_diff_next_x);

      // ------ adapted bilinear with 3x3 gaussian blur -------
      cur_p = p+x1;
      if(px_diff_prev_x > 1 || px_diff_next_x > 1 ){

        left_px_missing = (px_diff_prev_x > 1 && x1>0);
        right_px_missing = (px_diff_next_x > 1 && x1+1<w1);
        cur_p_left = cur_p-1;
        cur_p_right = cur_p+1;

        // ---- Interpolate current and left ----
        if(left_px_missing && !right_px_missing){
          *t++ = Weight1_1(*cur_p, *cur_p_left);
          //*t++ = Weight1_1(*cur_p, Weight1_3(*cur_p, *cur_p_left));
        }
        // ---- Interpolate current and right ----
        else if(right_px_missing && !left_px_missing){
          *t++ = Weight1_1(*cur_p, *cur_p_right);
          //*t++ = Weight1_1(*cur_p, Weight1_3(*cur_p, *cur_p_right));
        }
        // ---- Interpolate with Left and right pixels
        else{
          *t++ = Weight1_1(Weight1_1(*cur_p, *cur_p_left), Weight1_1(*cur_p, *cur_p_right));
        }

      }
      else{
        /// --- copy pixel ---
        *t++ = (*cur_p);

        /// Debug
        //occurence_pond[1] += 1;
      }

      /// save number of pixels to interpolate
      px_diff_prev_x = px_diff_next_x;

      // ------ next pixel ------
      rat += x_ratio;
    }
  }
}





/// Interpolation with left, right pixels, pseudo gaussian weighting for downscaling - operations on 16bits
void flip_Downscale_LeftRightGaussianFilter_OptimizedWidth320(SDL_Surface *src_surface, SDL_Surface *dst_surface, int new_w, int new_h){
  int w1=src_surface->w;
  int h1=src_surface->h;
  int w2=dst_surface->w;
  int h2=dst_surface->h;

  if(w1!=320){
    printf("src_surface->w (%d) != 320\n", src_surface->w);
    return;
  }

  //printf("src = %dx%d\n", w1, h1);
  int y_ratio = (int)((h1<<16)/h2);
  int y_padding = (RES_HW_SCREEN_VERTICAL-h2)/2;
  int y1;
  uint16_t *src_screen = (uint16_t *)src_surface->pixels;
  uint16_t *dst_screen = (uint16_t *)dst_surface->pixels;

  /* Interpolation */
  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }
    uint16_t* t = (uint16_t*)(dst_screen +
      (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2) );

    // ------ current and next y value ------
    y1 = ((i*y_ratio)>>16);
    uint16_t* p = (uint16_t*)(src_screen + (y1*w1) );

    for (int j=0;j<80;j++)
    {
      /* Horizontaly:
       * Before(4):
       * (a)(b)(c)(d)
       * After(3):
       * (aaab)(bc)(cddd)
       */
      uint16_t _a = *(p    );
      uint16_t _b = *(p + 1);
      uint16_t _c = *(p + 2);
      uint16_t _d = *(p + 3);
      *(t    ) = Weight3_1( _a, _b );
      *(t + 1) = Weight1_1( _b, _c );
      *(t + 2) = Weight1_3( _c, _d );

      // ------ next dst pixel ------
      t+=3;
      p+=4;
    }
  }
}





/// Interpolation with left, right pixels, pseudo gaussian weighting for downscaling - operations on 16bits
void flip_Downscale_OptimizedWidth320_mergeUpDown(SDL_Surface *src_surface, SDL_Surface *dst_surface, int new_w, int new_h){
  int w1=src_surface->w;
  int h1=src_surface->h;
  int w2=dst_surface->w;
  int h2=dst_surface->h;

  if(w1!=320){
    printf("src_surface->w (%d) != 320\n", src_surface->w);
    return;
  }

  //printf("src = %dx%d\n", w1, h1);
  int y_ratio = (int)((h1<<16)/h2);
  int y_padding = (RES_HW_SCREEN_VERTICAL-h2)/2;
  int y1=0, prev_y1=-1, prev_prev_y1=-2;
  uint16_t *src_screen = (uint16_t *)src_surface->pixels;
  uint16_t *dst_screen = (uint16_t *)dst_surface->pixels;

  uint16_t *prev_t, *t_init=dst_screen;

  /* Interpolation */
  for (int i=0;i<h2;i++)
  {
    if(i>=RES_HW_SCREEN_VERTICAL){
      continue;
    }

    prev_t = t_init;
    t_init = (uint16_t*)(dst_screen +
      (i+y_padding)*((w2>RES_HW_SCREEN_HORIZONTAL)?RES_HW_SCREEN_HORIZONTAL:w2) );
    uint16_t *t = t_init;

    // ------ current and next y value ------
    prev_prev_y1 = prev_y1;
    prev_y1 = y1;
    y1 = ((i*y_ratio)>>16);

    uint16_t* p = (uint16_t*)(src_screen + (y1*w1) );

    for (int j=0;j<80;j++)
    {
      /* Horizontaly:
       * Before(4):
       * (a)(b)(c)(d)
       * After(3):
       * (aaab)(bc)(cddd)
       */
      uint16_t _a = *(p    );
      uint16_t _b = *(p + 1);
      uint16_t _c = *(p + 2);
      uint16_t _d = *(p + 3);
      *(t    ) = Weight3_1( _a, _b );
      *(t + 1) = Weight1_1( _b, _c );
      *(t + 2) = Weight1_3( _c, _d );

      if(prev_y1 == prev_prev_y1 && y1 != prev_y1){
        //printf("we are here %d\n", ++count);
        *(prev_t    ) = Weight1_1(*(t    ), *(prev_t    ));
        *(prev_t + 1) = Weight1_1(*(t + 1), *(prev_t + 1));
        *(prev_t + 2) = Weight1_1(*(t + 2), *(prev_t + 2));
      }


      // ------ next dst pixel ------
      t+=3;
      prev_t+=3;
      p+=4;
    }
  }
}

void upscale_160x144_to_240x240_bilinearish(SDL_Surface *src_surface, SDL_Surface *dst_surface)
{
  if (src_surface->w != 160)
  {
    printf("src_surface->w (%d) != 160 \n", src_surface->w);
    return;
  }
  if (src_surface->h != 144)
  {
    printf("src_surface->h (%d) != 144 \n", src_surface->h);
    return;
  }

  uint16_t *Src16 = (uint16_t *) src_surface->pixels;
  uint16_t *Dst16 = (uint16_t *) dst_surface->pixels;

  // There are 80 blocks of 2 pixels horizontally, and 48 of 3 horizontally.
  // Horizontally: 240=80*3 160=80*2
  // Vertically: 240=48*5 144=48*3
  // Each block of 2*3 becomes 3x5.
  uint32_t BlockX, BlockY;
  uint16_t *BlockSrc;
  uint16_t *BlockDst;
  uint16_t _a, _b, _ab, __a, __b, __ab;
  for (BlockY = 0; BlockY < 48; BlockY++)
  {
    BlockSrc = Src16 + BlockY * 160 * 3;
    BlockDst = Dst16 + BlockY * 240 * 5;
    for (BlockX = 0; BlockX < 80; BlockX++)
    {
      /* Horizontaly:
       * Before(2):
       * (a)(b)
       * After(3):
       * (a)(ab)(b)
       */

      /* Verticaly:
       * Before(3):
       * (1)(2)(3)
       * After(5):
       * (1)(12)(2)(23)(3)
       */

      // -- Line 1 --
      _a = *(BlockSrc                          );
      _b = *(BlockSrc                       + 1);
      _ab = Weight1_1( _a,  _b);
      *(BlockDst                               ) = _a;
      *(BlockDst                            + 1) = _ab;
      *(BlockDst                            + 2) = _b;

      // -- Line 2 --
      __a = *(BlockSrc            + 160 * 1    );
      __b = *(BlockSrc            + 160 * 1 + 1);
      __ab = Weight1_1( __a,  __b);
      *(BlockDst                  + 240 * 1    ) = Weight1_1(_a, __a);
      *(BlockDst                  + 240 * 1 + 1) = Weight1_1(_ab, __ab);
      *(BlockDst                  + 240 * 1 + 2) = Weight1_1(_b, __b);

      // -- Line 3 --
      *(BlockDst                  + 240 * 2    ) = __a;
      *(BlockDst                  + 240 * 2 + 1) = __ab;
      *(BlockDst                  + 240 * 2 + 2) = __b;

      // -- Line 4 --
      _a = __a;
      _b = __b;
      _ab = __ab;
      __a = *(BlockSrc            + 160 * 2    );
      __b = *(BlockSrc            + 160 * 2 + 1);
      __ab = Weight1_1( __a,  __b);
      *(BlockDst                  + 240 * 3    ) = Weight1_1(_a, __a);
      *(BlockDst                  + 240 * 3 + 1) = Weight1_1(_ab, __ab);
      *(BlockDst                  + 240 * 3 + 2) = Weight1_1(_b, __b);

      // -- Line 5 --
      *(BlockDst                  + 240 * 4    ) = __a;
      *(BlockDst                  + 240 * 4 + 1) = __ab;
      *(BlockDst                  + 240 * 4 + 2) = __b;

      BlockSrc += 2;
      BlockDst += 3;
    }
  }
}


void upscale_160x144_to_240x216_bilinearish(SDL_Surface *src_surface, SDL_Surface *dst_surface)
{
  if (src_surface->w != 160)
  {
    printf("src_surface->w (%d) != 160 \n", src_surface->w);
    return;
  }
  if (src_surface->h != 144)
  {
    printf("src_surface->h (%d) != 144 \n", src_surface->h);
    return;
  }

  /* Y padding for centering */
  uint32_t y_padding = (240 - 216) / 2 + 1;

  uint16_t *Src16 = (uint16_t *) src_surface->pixels;
  uint16_t *Dst16 = ((uint16_t *) dst_surface->pixels) + y_padding * 240;

  // There are 80 blocks of 2 pixels horizontally, and 72 of 2 horizontally.
  // Horizontally: 240=80*3 160=80*2
  // Vertically: 216=72*3 144=72*2
  // Each block of 2*3 becomes 3x5.
  uint32_t BlockX, BlockY;
  uint16_t *BlockSrc;
  uint16_t *BlockDst;
  volatile uint16_t _a, _b, _ab, __a, __b, __ab;
  for (BlockY = 0; BlockY < 72; BlockY++)
  {

    BlockSrc = Src16 + BlockY * 160 * 2;
    BlockDst = Dst16 + BlockY * 240 * 3;
    for (BlockX = 0; BlockX < 80; BlockX++)
    {
      /* Horizontaly:
       * Before(2):
       * (a)(b)
       * After(3):
       * (a)(ab)(b)
       */

      /* Verticaly:
       * Before(2):
       * (1)(2)
       * After(3):
       * (1)(12)(2)
       */

      // -- Line 1 --
      _a = *(BlockSrc                    );
      _b = *(BlockSrc                 + 1);
      _ab = Weight1_1( _a,  _b);
      *(BlockDst                         ) = _a;
      *(BlockDst                      + 1) = _ab;
      *(BlockDst                      + 2) = _b;

      // -- Line 2 --
      __a = *(BlockSrc      + 160 * 1    );
      __b = *(BlockSrc      + 160 * 1 + 1);
      __ab = Weight1_1( __a,  __b);
      *(BlockDst            + 240 * 1    ) = Weight1_1(_a, __a);
      *(BlockDst            + 240 * 1 + 1) = Weight1_1(_ab, __ab);
      *(BlockDst            + 240 * 1 + 2) = Weight1_1(_b, __b);

      // -- Line 3 --
      *(BlockDst            + 240 * 2    ) = __a;
      *(BlockDst            + 240 * 2 + 1) = __ab;
      *(BlockDst            + 240 * 2 + 2) = __b;

      BlockSrc += 2;
      BlockDst += 3;
    }
  }
}

void SDL_Rotate_270(SDL_Surface * hw_surface, SDL_Surface * virtual_hw_surface){
    int i, j;
    uint16_t *source_pixels = (uint16_t*) virtual_hw_surface->pixels;
    uint16_t *dest_pixels = (uint16_t*) hw_surface->pixels;

    /// --- Checking for right pixel format ---
    //printf("Source bpb = %d, Dest bpb = %d\n", virtual_hw_surface->format->BitsPerPixel, hw_surface->format->BitsPerPixel);
    if(virtual_hw_surface->format->BitsPerPixel != 16){
	printf("Error in SDL_FastBlit, Wrong virtual_hw_surface pixel format: %d bpb, expected: 16 bpb\n", virtual_hw_surface->format->BitsPerPixel);
	return;
    }
    if(hw_surface->format->BitsPerPixel != 16){
	printf("Error in SDL_FastBlit, Wrong hw_surface pixel format: %d bpb, expected: 16 bpb\n", hw_surface->format->BitsPerPixel);
	return;
    }

    /// --- Checking if same dimensions ---
    if(hw_surface->w != virtual_hw_surface->w || hw_surface->h != virtual_hw_surface->h){
	printf("Error in SDL_FastBlit, hw_surface (%dx%d) and virtual_hw_surface (%dx%d) have different dimensions\n",
	       hw_surface->w, hw_surface->h, virtual_hw_surface->w, virtual_hw_surface->h);
	return;
    }

	/// --- Pixel copy and rotation (270) ---
	uint16_t *cur_p_src, *cur_p_dst;
	for(i=0; i<virtual_hw_surface->h; i++){
		for(j=0; j<virtual_hw_surface->w; j++){
			cur_p_src = source_pixels + i*virtual_hw_surface->w + j;
			cur_p_dst = dest_pixels + (hw_surface->h-1-j)*hw_surface->w + i;
			*cur_p_dst = *cur_p_src;
		}
	}
}






void scale_for_gg(SDL_Surface *src_surface, 
                  SDL_Surface *dst_surface, 
                  ENUM_ASPECT_RATIOS_TYPES aspect_ratio){
  //printf("In %s\n", __func__);

  switch(aspect_ratio){
  case ASPECT_RATIOS_TYPE_STRETCHED:
    upscale_160x144_to_240x240_bilinearish(src_surface, dst_surface);
    break;

  case ASPECT_RATIOS_TYPE_MANUAL:
    /*;uint32_t h_scaled = src_surface->h*RES_HW_SCREEN_HORIZONTAL/src_surface->w;
    ;uint32_t h_zoomed = h_scaled + aspect_ratio_factor_percent*(RES_HW_SCREEN_VERTICAL - h_scaled)/100;
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
                    MAX(src_surface->w*h_zoomed/src_surface->h, RES_HW_SCREEN_HORIZONTAL),
                    MIN(h_zoomed, RES_HW_SCREEN_VERTICAL));*/

    ;uint32_t h_min = src_surface->h;
    ;uint32_t h_zoomed = h_min + aspect_ratio_factor_percent*(RES_HW_SCREEN_VERTICAL - h_min)/100;
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
                    src_surface->w*h_zoomed/src_surface->h,
                    MIN(h_zoomed, RES_HW_SCREEN_VERTICAL));
    break;

  case ASPECT_RATIOS_TYPE_CROPPED:
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              src_surface->w*RES_HW_SCREEN_VERTICAL/src_surface->h,
              RES_HW_SCREEN_VERTICAL);
    break;

  case ASPECT_RATIOS_TYPE_SCALED:
    upscale_160x144_to_240x216_bilinearish(src_surface, dst_surface);
    break;

  default:
    printf("Wrong aspect ratio value: %d\n", aspect_ratio);
    aspect_ratio = ASPECT_RATIOS_TYPE_STRETCHED;
    break;
  }
}


void scale_for_SMS(SDL_Surface *src_surface, 
                    SDL_Surface *dst_surface, 
                    ENUM_ASPECT_RATIOS_TYPES aspect_ratio){
  //printf("In %s\n", __func__);

  switch(aspect_ratio){
  case ASPECT_RATIOS_TYPE_STRETCHED:
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              RES_HW_SCREEN_HORIZONTAL,
              RES_HW_SCREEN_VERTICAL);
    break;

  case ASPECT_RATIOS_TYPE_MANUAL:
    ;uint32_t h_scaled = MIN(src_surface->h*RES_HW_SCREEN_HORIZONTAL/src_surface->w,
           RES_HW_SCREEN_VERTICAL);
    uint32_t h_zoomed = MIN(h_scaled + aspect_ratio_factor_percent*(RES_HW_SCREEN_VERTICAL - h_scaled)/100,
          RES_HW_SCREEN_VERTICAL);
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
                    MAX(src_surface->w*h_zoomed/src_surface->h, RES_HW_SCREEN_HORIZONTAL),
                    MIN(h_zoomed, RES_HW_SCREEN_VERTICAL));
    break;

  case ASPECT_RATIOS_TYPE_CROPPED:
      flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              src_surface->w,
              src_surface->h);
    break;

  case ASPECT_RATIOS_TYPE_SCALED:
    flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              RES_HW_SCREEN_HORIZONTAL,
              src_surface->h*RES_HW_SCREEN_HORIZONTAL/src_surface->w);
    break;

  default:
    printf("Wrong aspect ratio value: %d\n", aspect_ratio);
    aspect_ratio = ASPECT_RATIOS_TYPE_STRETCHED;
    break;
  }
}


void scale_for_genesis(SDL_Surface *src_surface, 
                        SDL_Surface *dst_surface, 
                        ENUM_ASPECT_RATIOS_TYPES aspect_ratio){
  //printf("In %s\n", __func__);
  uint16_t hres_max;

  switch(aspect_ratio){
  case ASPECT_RATIOS_TYPE_STRETCHED:
    if(src_surface->w == 320 && src_surface->h < RES_HW_SCREEN_VERTICAL){
      flip_Downscale_OptimizedWidth320_mergeUpDown(src_surface, dst_surface,
                     RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);
    }
    else if(src_surface->w == 320){
      flip_Downscale_LeftRightGaussianFilter_OptimizedWidth320(src_surface, dst_surface,
                     RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);
    }
    else{
      flip_Downscale_LeftRightGaussianFilter_Optimized(src_surface, dst_surface,
                   RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);
      /*flip_Downscale_LeftRightGaussianFilter(src_surface, hw_screen,
        RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);*/
    }
    break;

  case ASPECT_RATIOS_TYPE_MANUAL:
    hres_max= MIN(RES_HW_SCREEN_VERTICAL, src_surface->h);
    ;uint32_t h_scaled = MIN(src_surface->h*RES_HW_SCREEN_HORIZONTAL/src_surface->w,
           RES_HW_SCREEN_VERTICAL);
    uint32_t h_zoomed = MIN(h_scaled + aspect_ratio_factor_percent*(hres_max - h_scaled)/100,
          RES_HW_SCREEN_VERTICAL);
    flip_NNOptimized_LeftRightUpDownBilinear_Optimized8(src_surface, dst_surface,
                    MAX(src_surface->w*h_zoomed/src_surface->h, RES_HW_SCREEN_HORIZONTAL),
                    MIN(h_zoomed, RES_HW_SCREEN_VERTICAL));
    break;

  case ASPECT_RATIOS_TYPE_CROPPED:
    /*flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              MAX(src_surface->w*RES_HW_SCREEN_VERTICAL/src_surface->h, RES_HW_SCREEN_HORIZONTAL),
              RES_HW_SCREEN_VERTICAL);*/
      hres_max= MIN(RES_HW_SCREEN_VERTICAL, src_surface->h);
      flip_NNOptimized_AllowOutOfScreen(src_surface, dst_surface,
              MAX(src_surface->w*hres_max/src_surface->h, RES_HW_SCREEN_HORIZONTAL),
              hres_max);
    break;

  case ASPECT_RATIOS_TYPE_SCALED:
    flip_NNOptimized_LeftRightUpDownBilinear_Optimized8(src_surface, dst_surface,
                    RES_HW_SCREEN_HORIZONTAL,
                    MIN(src_surface->h*RES_HW_SCREEN_HORIZONTAL/src_surface->w, RES_HW_SCREEN_VERTICAL));
    break;

  default:
    printf("Wrong aspect ratio value: %d\n", aspect_ratio);
    aspect_ratio = ASPECT_RATIOS_TYPE_STRETCHED;
    flip_NNOptimized_LeftRightUpDownBilinear_Optimized8(src_surface, dst_surface,
                    RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);
    break;
  }
}







static int clear_buf_cnt, clear_stat_cnt;

void plat_video_set_size(int w, int h)
{
	if (area.w != w || area.h != h) {
		area = (struct area) { w, h };

		if (plat_sdl_change_video_mode(w, h, 0) < 0) {
			// failed, revert to original resolution
			plat_sdl_change_video_mode(g_screen_width, g_screen_height, 0);
			w = g_screen_width, h = g_screen_height;
		}
		if (!plat_sdl_overlay && !plat_sdl_gl_active) {
			g_screen_width = w;
			g_screen_height = h;
			g_screen_ppitch = w;
			g_screen_ptr = plat_sdl_screen->pixels;
		}
	}
}

void plat_video_flip(void)
{
	if (plat_sdl_overlay != NULL) {
		SDL_Rect dstrect =
			{ 0, 0, plat_sdl_screen->w, plat_sdl_screen->h };
		SDL_LockYUVOverlay(plat_sdl_overlay);
		rgb565_to_uyvy(plat_sdl_overlay->pixels[0], shadow_fb,
				area.w, area.h, g_screen_ppitch,
				plat_sdl_overlay->w >= 2*area.w);
		SDL_UnlockYUVOverlay(plat_sdl_overlay);
		SDL_DisplayYUVOverlay(plat_sdl_overlay, &dstrect);
	}
	else if (plat_sdl_gl_active) {
		gl_flip(shadow_fb, g_screen_ppitch, g_screen_height);
	}
	/*else {
		if (SDL_MUSTLOCK(plat_sdl_screen)) {
			SDL_UnlockSurface(plat_sdl_screen);
			SDL_Flip(plat_sdl_screen);
			SDL_LockSurface(plat_sdl_screen);
		} else
			SDL_Flip(plat_sdl_screen);
		g_screen_ptr = plat_sdl_screen->pixels;
		plat_video_set_buffer(g_screen_ptr);
		if (clear_buf_cnt) {
			memset(g_screen_ptr, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
			clear_buf_cnt--;
		}
	}*/
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_UnlockSurface(plat_sdl_screen);

		/* Surface with game data */
		SDL_Surface *game_surface;

		/* Sega Game Gear -> 160*144 res in 320*240 surface */
		//if ((PicoIn.AHW & PAHW_SMS) && (Pico.m.hardware & 0x3) == 0x3){
		if ((PicoIn.AHW & PAHW_SMS) && (Pico.m.hardware & 0x01)){

			/* Copy Game Gear game pixels */
			int offset_y = (plat_sdl_screen->h - gg_game_screen->h)/2;
			int offset_x = (plat_sdl_screen->w - gg_game_screen->w)/2 - 1;
			int y;
			for(y=0; y<gg_game_screen->h; y++){
				memcpy((uint16_t*)gg_game_screen->pixels + gg_game_screen->w*y,
				       (uint16_t*)plat_sdl_screen->pixels + plat_sdl_screen->w*(y+offset_y) + offset_x,
				       gg_game_screen->w*sizeof(uint16_t));
			}

			game_surface = gg_game_screen;
		}
		/* Sega Master System -> 256*192 res in 320*240 surface */
		else if (PicoIn.AHW & PAHW_SMS){

			/* Copy sms game pixels */
			int offset_y = (plat_sdl_screen->h - sms_game_screen->h)/2;
			int offset_x = (plat_sdl_screen->w - sms_game_screen->w)/2 + 1;
			int y;
			for(y=0; y<sms_game_screen->h; y++){
				memcpy((uint16_t*)sms_game_screen->pixels + sms_game_screen->w*y,
				       (uint16_t*)plat_sdl_screen->pixels + plat_sdl_screen->w*(y+offset_y) + offset_x,
				       sms_game_screen->w*sizeof(uint16_t));
			}

			game_surface = sms_game_screen;
		}
		else{
			game_surface = plat_sdl_screen;
		}


		  /// --------------Optimized Flip depending on aspect ratio -------------
		static int prev_aspect_ratio;
		if(prev_aspect_ratio != aspect_ratio || need_screen_cleared){
			/*printf("aspect ratio changed: %s\n", aspect_ratio_name[aspect_ratio]);
      printf("game_surface res = %dx%d\n", game_surface->w, game_surface->h);*/
		  clear_screen(virtual_hw_screen, 0);
		  prev_aspect_ratio = aspect_ratio;
		  need_screen_cleared = 0;
		}

    /** Rescale for console */
    /** Game Gear */
    if((PicoIn.AHW & PAHW_SMS) && (Pico.m.hardware & 0x01)){
      scale_for_gg(game_surface, virtual_hw_screen, aspect_ratio);
    }
    /** SMS */
    else if(PicoIn.AHW & PAHW_SMS){
      scale_for_SMS(game_surface, virtual_hw_screen, aspect_ratio);
    }
    /** Genesis */
    else{
      scale_for_genesis(game_surface, virtual_hw_screen, aspect_ratio);
    }

		// Rotate
		//SDL_Rotate_270(hw_screen, virtual_hw_screen);
		//SDL_BlitSurface(virtual_hw_screen, NULL, hw_screen, NULL);
		memcpy(hw_screen->pixels, virtual_hw_screen->pixels, hw_screen->w*hw_screen->h*sizeof(uint16_t));

		/// --- Real Flip ---
		SDL_Flip(hw_screen);


		/*g_screen_ptr = plat_sdl_screen->pixels;
		PicoDrawSetOutBuf(g_screen_ptr, g_screen_ppitch * 2);*/
	}
	/*if (clear_stat_cnt) {
		unsigned short *d = (unsigned short *)g_screen_ptr + g_screen_ppitch * g_screen_height;
		int l = g_screen_ppitch * 8;
		memset((int *)(d - l), 0, l * 2);
		clear_stat_cnt--;
	}*/
}

void plat_video_wait_vsync(void)
{
}

void plat_video_clear_status(void)
{
	clear_stat_cnt = 3; // do it thrice in case of triple buffering
}

void plat_video_clear_buffers(void)
{
	if (plat_sdl_overlay != NULL || plat_sdl_gl_active)
		memset(shadow_fb, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
	else {
		memset(g_screen_ptr, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
		clear_buf_cnt = 3; // do it thrice in case of triple buffering
	}
}

void plat_video_menu_enter(int is_rom_loaded)
{
	if (SDL_MUSTLOCK(plat_sdl_screen))
		SDL_UnlockSurface(plat_sdl_screen);
	plat_sdl_change_video_mode(g_menuscreen_w, g_menuscreen_h, 1);
	g_screen_ptr = shadow_fb;
	plat_video_set_buffer(g_screen_ptr);
}

void plat_video_menu_begin(void)
{
	if (plat_sdl_overlay != NULL || plat_sdl_gl_active) {
		g_menuscreen_ptr = shadow_fb;
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_LockSurface(plat_sdl_screen);
		g_menuscreen_ptr = plat_sdl_screen->pixels;
	}
}

void plat_video_menu_end(void)
{
	if (plat_sdl_overlay != NULL) {
		SDL_Rect dstrect =
			{ 0, 0, plat_sdl_screen->w, plat_sdl_screen->h };

		SDL_LockYUVOverlay(plat_sdl_overlay);
		rgb565_to_uyvy(plat_sdl_overlay->pixels[0], shadow_fb,
			g_menuscreen_w, g_menuscreen_h, g_menuscreen_pp, 0);
		SDL_UnlockYUVOverlay(plat_sdl_overlay);

		SDL_DisplayYUVOverlay(plat_sdl_overlay, &dstrect);
	}
	else if (plat_sdl_gl_active) {
		gl_flip(g_menuscreen_ptr, g_menuscreen_pp, g_menuscreen_h);
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_UnlockSurface(plat_sdl_screen);
		flip_NNOptimized_LeftAndRightBilinear(plat_sdl_screen, virtual_hw_screen, RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL);
		
    memcpy(hw_screen->pixels, virtual_hw_screen->pixels, hw_screen->w*hw_screen->h*sizeof(uint16_t));
    SDL_Flip(hw_screen);
		//SDL_Rotate_270(hw_screen, virtual_hw_screen);
		//SDL_Flip(plat_sdl_screen);
	}
	g_menuscreen_ptr = NULL;
}

void plat_video_menu_leave(void)
{
}

void plat_video_loop_prepare(void)
{
	// take over any new vout settings
	plat_sdl_change_video_mode(g_menuscreen_w, g_menuscreen_h, 0);
	// switch over to scaled output if available, but keep the aspect ratio
	if (plat_sdl_overlay != NULL || plat_sdl_gl_active) {
		g_screen_width = (240 * g_menuscreen_w / g_menuscreen_h) & ~1;
		g_screen_height = 240;
		g_screen_ppitch = g_screen_width;
		plat_sdl_change_video_mode(g_screen_width, g_screen_height, 0);
		g_screen_ptr = shadow_fb;
	}
	else {
		g_screen_width = g_menuscreen_w;
		g_screen_height = g_menuscreen_h;
		g_screen_ppitch = g_menuscreen_pp;
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_LockSurface(plat_sdl_screen);
		g_screen_ptr = plat_sdl_screen->pixels;
	}
	plat_video_set_buffer(g_screen_ptr);
	plat_video_set_size(g_screen_width, g_screen_height);
}

void plat_early_init(void)
{
}

static void plat_sdl_quit(void)
{
	// for now..
	engineState = PGS_Quit;
	//exit(1);
}

void plat_init(void)
{
	int shadow_size;
	int ret;

	ret = plat_sdl_init();
	if (ret != 0)
		exit(1);
	SDL_ShowCursor(0);
#if defined(__RG350__) || defined(__GCW0__) || defined(__OPENDINGUX__)
	// opendingux on JZ47x0 may falsely report a HW overlay, fix to window
	plat_target.vout_method = 0;
#endif

	if(TTF_Init())
	{
		fprintf(stderr, "Error TTF_Init: %s\n", TTF_GetError());
		exit(EXIT_FAILURE);
	}

	hw_screen = SDL_SetVideoMode(RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL, 16, SDL_FULLSCREEN | SDL_HWSURFACE | SDL_DOUBLEBUF);
	if(hw_screen == NULL)
	{
		fprintf(stderr, "Error SDL_SetVideoMode: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	plat_sdl_quit_cb = plat_sdl_quit;

	SDL_WM_SetCaption("PicoDrive " VERSION, NULL);

	virtual_hw_screen = SDL_CreateRGBSurface(SDL_SWSURFACE,
      RES_HW_SCREEN_HORIZONTAL, RES_HW_SCREEN_VERTICAL, 16, 0xFFFF, 0xFFFF, 0xFFFF, 0);
  if (virtual_hw_screen == NULL) {
    fprintf(stderr, "virtual_hw_screen failed: %s\n", SDL_GetError());
  }

  sms_game_screen = SDL_CreateRGBSurface(SDL_SWSURFACE,
      256, 192, 16, 0xFFFF, 0xFFFF, 0xFFFF, 0);
  if (sms_game_screen == NULL) {
    fprintf(stderr, "sms_game_screen failed: %s\n", SDL_GetError());
  }

  gg_game_screen = SDL_CreateRGBSurface(SDL_SWSURFACE,
      160, 144, 16, 0xFFFF, 0xFFFF, 0xFFFF, 0);
  if (gg_game_screen == NULL) {
    fprintf(stderr, "gg_game_screen failed: %s\n", SDL_GetError());
  }

	g_menuscreen_w = plat_sdl_screen->w;
	g_menuscreen_h = plat_sdl_screen->h;
	g_menuscreen_pp = g_menuscreen_w;
	g_menuscreen_ptr = NULL;

	shadow_size = g_menuscreen_w * g_menuscreen_h * 2;
	if (shadow_size < 320 * 480 * 2)
		shadow_size = 320 * 480 * 2;

	shadow_fb = calloc(1, shadow_size);
	g_menubg_ptr = calloc(1, shadow_size);
	if (shadow_fb == NULL || g_menubg_ptr == NULL) {
		fprintf(stderr, "OOM\n");
		exit(1);
	}

	g_screen_width = 320;
	g_screen_height = 240;
	g_screen_ppitch = 320;
	g_screen_ptr = shadow_fb;

	in_sdl_platform_data.kmap_size = in_sdl_key_map_sz,
	in_sdl_platform_data.jmap_size = in_sdl_joy_map_sz,
	in_sdl_platform_data.key_names = *in_sdl_key_names,
  /** Done later depending on SMS or genesis */
	/*in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler);
	in_probe();*/

	init_menu_SDL();
	bgr_to_uyvy_init();
}

void plat_set_sms_input(void){
  in_sdl_init(&in_sdl_platform_data_SMS, plat_sdl_event_handler);
  in_probe();
}

void plat_set_genesis_input(void){
  in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler);
  in_probe();
}

void plat_finish(void)
{
	SDL_FreeSurface(virtual_hw_screen);
	SDL_FreeSurface(sms_game_screen);
	SDL_FreeSurface(gg_game_screen);
	deinit_menu_SDL();
	free(shadow_fb);
	shadow_fb = NULL;
	free(g_menubg_ptr);
	g_menubg_ptr = NULL;
	TTF_Quit();
	plat_sdl_finish();
}
