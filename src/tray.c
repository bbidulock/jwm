/**
 * @file tray.c
 * @author Joe Wingbermuehle
 * @date 2004-2006
 *
 * @brief Tray functions.
 *
 */

#include "jwm.h"
#include "tray.h"
#include "color.h"
#include "main.h"
#include "pager.h"
#include "cursor.h"
#include "error.h"
#include "taskbar.h"
#include "menu.h"
#include "timing.h"
#include "screen.h"
#include "settings.h"
#include "event.h"
#include "client.h"
#include "misc.h"

#define DEFAULT_TRAY_WIDTH 32
#define DEFAULT_TRAY_HEIGHT 32

static TrayType *trays;
static unsigned int trayCount;

static void HandleTrayExpose(TrayType *tp, const XExposeEvent *event);
static void HandleTrayEnterNotify(TrayType *tp, const XCrossingEvent *event);

static TrayComponentType *GetTrayComponent(TrayType *tp, int x, int y);
static void HandleTrayButtonPress(TrayType *tp, const XButtonEvent *event);
static void HandleTrayButtonRelease(TrayType *tp, const XButtonEvent *event);
static void HandleTrayMotionNotify(TrayType *tp, const XMotionEvent *event);

static void ComputeTraySize(TrayType *tp);
static int ComputeMaxWidth(TrayType *tp);
static int ComputeTotalWidth(TrayType *tp);
static int ComputeMaxHeight(TrayType *tp);
static int ComputeTotalHeight(TrayType *tp);
static char CheckHorizontalFill(TrayType *tp);
static char CheckVerticalFill(TrayType *tp);
static void LayoutTray(TrayType *tp, int *variableSize,
                       int *variableRemainder);

static void SignalTray(const TimeType *now, int x, int y, Window w,
                       void *data);


/** Initialize tray data. */
void InitializeTray(void)
{
   trays = NULL;
   trayCount = 0;
}

/** Startup trays. */
void StartupTray(void)
{

   XSetWindowAttributes attr;
   Atom atom;
   unsigned long attrMask;
   TrayType *tp;
   TrayComponentType *cp;
   int variableSize;
   int variableRemainder;
   int width, height;
   int xoffset, yoffset;

   for(tp = trays; tp; tp = tp->next) {

      LayoutTray(tp, &variableSize, &variableRemainder);

      /* Create the tray window. */
      /* The window is created larger for a border. */
      attrMask = CWOverrideRedirect;
      attr.override_redirect = True;

      /* We can't use PointerMotionHintMask since the exact position
       * of the mouse on the tray is important for popups. */
      attrMask |= CWEventMask;
      attr.event_mask
         = ButtonPressMask
         | ButtonReleaseMask
         | SubstructureNotifyMask
         | ExposureMask
         | KeyPressMask
         | KeyReleaseMask
         | EnterWindowMask
         | PointerMotionMask;

      attrMask |= CWBackPixel;
      attr.background_pixel = colors[COLOR_TRAY_BG2];

      attrMask |= CWBorderPixel;
      attr.border_pixel = colors[COLOR_TRAY_OUTLINE];

      Assert(tp->width > 0);
      Assert(tp->height > 0);
      tp->window = JXCreateWindow(display, rootWindow,
                                  tp->x, tp->y, tp->width, tp->height,
                                  TRAY_BORDER_SIZE,
                                  rootVisual.depth, InputOutput,
                                  rootVisual.visual, attrMask, &attr);

      if(settings.trayOpacity < UINT_MAX) {
         /* Can't use atoms yet as it hasn't been initialized. */
         atom = JXInternAtom(display, opacityAtom, False);
         JXChangeProperty(display, tp->window, atom, XA_CARDINAL, 32,
                          PropModeReplace,
                          (unsigned char*)&settings.trayOpacity, 1);
      }

      SetDefaultCursor(tp->window);

      /* Create and layout items on the tray. */
      xoffset = 0;
      yoffset = 0;
      for(cp = tp->components; cp; cp = cp->next) {

         if(cp->Create) {
            if(tp->layout == LAYOUT_HORIZONTAL) {
               height = tp->height;
               width = cp->width;
               if(width == 0) {
                  width = variableSize;
                  if(variableRemainder) {
                     width += 1;
                     variableRemainder -= 1;
                  }
               }
            } else {
               width = tp->width;
               height = cp->height;
               if(height == 0) {
                  height = variableSize;
                  if(variableRemainder) {
                     height += 1;
                     variableRemainder -= 1;
                  }
               }
            }
            cp->width = Max(1, width);
            cp->height = Max(1, height);
            (cp->Create)(cp);
         }

         cp->x = xoffset;
         cp->y = yoffset;
         cp->screenx = tp->x + xoffset;
         cp->screeny = tp->y + yoffset;

         if(cp->window != None) {
            JXReparentWindow(display, cp->window, tp->window,
                             xoffset, yoffset);
         }

         if(tp->layout == LAYOUT_HORIZONTAL) {
            xoffset += cp->width;
         } else {
            yoffset += cp->height;
         }
      }

      /* Show the tray. */
      JXMapWindow(display, tp->window);

      trayCount += 1;

   }

   UpdatePager();
   UpdateTaskBar();

}

/** Shutdown trays. */
void ShutdownTray(void)
{

   TrayType *tp;
   TrayComponentType *cp;

   for(tp = trays; tp; tp = tp->next) {
      for(cp = tp->components; cp; cp = cp->next) {
         if(cp->Destroy) {
            (cp->Destroy)(cp);
         }
      }
      JXDestroyWindow(display, tp->window);
   }

}

/** Destroy tray data. */
void DestroyTray(void)
{

   TrayType *tp;
   TrayComponentType *cp;

   while(trays) {
      tp = trays->next;
      UnregisterCallback(SignalTray, trays);
      while(trays->components) {
         cp = trays->components->next;
         Release(trays->components);
         trays->components = cp;
      }
      Release(trays);

      trays = tp;
   }

}

/** Create an empty tray. */
TrayType *CreateTray(void)
{

   TrayType *tp;

   tp = Allocate(sizeof(TrayType));

   tp->requestedX = 0;
   tp->requestedY = -1;
   tp->x = 0;
   tp->y = -1;
   tp->requestedWidth = 0;
   tp->requestedHeight = 0;
   tp->width = 0;
   tp->height = 0;
   tp->layer = DEFAULT_TRAY_LAYER;
   tp->layout = LAYOUT_HORIZONTAL;
   tp->valign = TALIGN_FIXED;
   tp->halign = TALIGN_FIXED;

   tp->autoHide = THIDE_OFF;
   tp->hidden = 0;

   tp->window = None;

   tp->components = NULL;
   tp->componentsTail = NULL;

   tp->next = trays;
   trays = tp;

   RegisterCallback(100, SignalTray, tp);

   return tp;

}

/** Create an empty tray component. */
TrayComponentType *CreateTrayComponent(void)
{

   TrayComponentType *cp;

   cp = Allocate(sizeof(TrayComponentType));

   cp->tray = NULL;
   cp->object = NULL;

   cp->x = 0;
   cp->y = 0;
   cp->requestedWidth = 0;
   cp->requestedHeight = 0;
   cp->width = 0;
   cp->height = 0;
   cp->grabbed = 0;

   cp->window = None;
   cp->pixmap = None;

   cp->Create = NULL;
   cp->Destroy = NULL;

   cp->SetSize = NULL;
   cp->Resize = NULL;

   cp->ProcessButtonPress = NULL;
   cp->ProcessButtonRelease = NULL;
   cp->ProcessMotionEvent = NULL;
   cp->Redraw = NULL;

   cp->next = NULL;

   return cp;

}

/** Add a tray component to a tray. */
void AddTrayComponent(TrayType *tp, TrayComponentType *cp)
{

   Assert(tp);
   Assert(cp);

   cp->tray = tp;

   if(tp->componentsTail) {
      tp->componentsTail->next = cp;
   } else {
      tp->components = cp;
   }
   tp->componentsTail = cp;
   cp->next = NULL;

}

/** Compute the max component width. */
int ComputeMaxWidth(TrayType *tp)
{

   TrayComponentType *cp;
   int result;
   int temp;

   result = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      temp = cp->width;
      if(temp > 0) {
         if(temp > result) {
            result = temp;
         }
      }
   }

   return result;

}

/** Compute the total width of a tray. */
int ComputeTotalWidth(TrayType *tp)
{

   TrayComponentType *cp;
   int result;

   result = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      result += cp->width;
   }

   return result;

}

/** Compute the max component height. */
int ComputeMaxHeight(TrayType *tp)
{

   TrayComponentType *cp;
   int result;
   int temp;

   result = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      temp = cp->height;
      if(temp > 0) {
         if(temp > result) {
            result = temp;
         }
      }
   }

   return result;

}

/** Compute the total height of a tray. */
int ComputeTotalHeight(TrayType *tp)
{

   TrayComponentType *cp;
   int result;

   result = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      result += cp->height;
   }

   return result;

}

/** Check if the tray fills the screen horizontally. */
char CheckHorizontalFill(TrayType *tp)
{

   TrayComponentType *cp;

   for(cp = tp->components; cp; cp = cp->next) {
      if(cp->width == 0) {
         return 1;
      }
   }

   return 0;

}

/** Check if the tray fills the screen vertically. */
char CheckVerticalFill(TrayType *tp)
{

   TrayComponentType *cp;

   for(cp = tp->components; cp; cp = cp->next) {
      if(cp->height == 0) {
         return 1;
      }
   }

   return 0;

}

/** Compute the size of a tray. */
void ComputeTraySize(TrayType *tp)
{

   TrayComponentType *cp;
   const ScreenType *sp;
   int x, y;

   /* Determine the first dimension. */
   if(tp->layout == LAYOUT_HORIZONTAL) {

      if(tp->height == 0) {
         tp->height = ComputeMaxHeight(tp);
      }
      if(tp->height == 0) {
         tp->height = DEFAULT_TRAY_HEIGHT;
      }

   } else {

      if(tp->width == 0) {
         tp->width = ComputeMaxWidth(tp);
      }
      if(tp->width == 0) {
         tp->width = DEFAULT_TRAY_WIDTH;
      }

   }

   /* Now at least one size is known. Inform the components. */
   for(cp = tp->components; cp; cp = cp->next) {
      if(cp->SetSize) {
         if(tp->layout == LAYOUT_HORIZONTAL) {
            (cp->SetSize)(cp, 0, tp->height);
         } else {
            (cp->SetSize)(cp, tp->width, 0);
         }
      }
   }

   /* Initialize the coordinates. */
   tp->x = tp->requestedX;
   tp->y = tp->requestedY;

   /* Determine on which screen the tray will reside. */
   switch(tp->valign) {
   case TALIGN_TOP:
      y = 0;
      break;
   case TALIGN_BOTTOM:
      y = rootHeight - 1;
      break;
   case TALIGN_CENTER:
      y = 1 + rootHeight / 2;
      break;
   default:
      if(tp->y < 0) {
         y = rootHeight + tp->y;
      } else {
         y = tp->y;
      }
      break;
   }
   switch(tp->halign) {
   case TALIGN_LEFT:
      x = 0;
      break;
   case TALIGN_RIGHT:
      x = rootWidth - 1;
      break;
   case TALIGN_CENTER:
      x = 1 + rootWidth / 2;
      break;
   default:
      if(tp->x < 0) {
         x = rootWidth + tp->x;
      } else {
         x = tp->x;
      }
      break;
   }
   sp = GetCurrentScreen(x, y);

   /* Determine the missing dimension. */
   if(tp->layout == LAYOUT_HORIZONTAL) {
      if(tp->width == 0) {
         if(CheckHorizontalFill(tp)) {
            tp->width = sp->width;
         } else {
            tp->width = ComputeTotalWidth(tp);
         }
         if(tp->width == 0) {
            tp->width = DEFAULT_TRAY_WIDTH;
         }
      }
   } else {
      if(tp->height == 0) {
         if(CheckVerticalFill(tp)) {
            tp->height = sp->height;
         } else {
            tp->height = ComputeTotalHeight(tp);
         }
         if(tp->height == 0) {
            tp->height = DEFAULT_TRAY_HEIGHT;
         }
      }
   }

   /* Compute the tray location. */
   switch(tp->valign) {
   case TALIGN_TOP:
      tp->y = sp->y - TRAY_BORDER_SIZE;
      break;
   case TALIGN_BOTTOM:
      tp->y = sp->y + sp->height - tp->height + TRAY_BORDER_SIZE;
      break;
   case TALIGN_CENTER:
      tp->y = sp->y + (sp->height - tp->height) / 2;
      break;
   default:
      if(tp->y < 0) {
         tp->y = sp->y + sp->height - tp->height + TRAY_BORDER_SIZE;
      } else {
         tp->y -= TRAY_BORDER_SIZE;
      }
      break;
   }

   switch(tp->halign) {
   case TALIGN_LEFT:
      tp->x = sp->x - TRAY_BORDER_SIZE;
      break;
   case TALIGN_RIGHT:
      tp->x = sp->x + sp->width - tp->width + TRAY_BORDER_SIZE;
      break;
   case TALIGN_CENTER:
      tp->x = sp->x + (sp->width - tp->width) / 2;
      break;
   default:
      if(tp->x < 0) {
         tp->x = sp->x + sp->width - tp->width + TRAY_BORDER_SIZE;
      } else {
         tp->x -= TRAY_BORDER_SIZE;
      }
      break;
   }

}

/** Display a tray (for autohide). */
void ShowTray(TrayType *tp)
{

   Window win1, win2;
   int winx, winy;
   unsigned int mask;
   int mousex, mousey;

   if(tp->hidden) {

      tp->hidden = 0;
      JXMoveWindow(display, tp->window, tp->x, tp->y);

      JXQueryPointer(display, rootWindow, &win1, &win2,
                     &mousex, &mousey, &winx, &winy, &mask);
      SetMousePosition(mousex, mousey, win2);

   }

}

/** Show all trays. */
void ShowAllTrays(void)
{

   TrayType *tp;

   if(shouldExit) {
      return;
   }

   for(tp = trays; tp; tp = tp->next) {
      ShowTray(tp);
   }

}

/** Hide a tray (for autohide). */
void HideTray(TrayType *tp)
{

   const ScreenType *sp;
   int x, y;

   /* Don't hide if the tray is raised. */
   if(tp->autoHide & THIDE_RAISED) {
      return;
   }

   tp->hidden = 1;

   /* Determine where to move the tray. */
   sp = GetCurrentScreen(tp->x, tp->y);
   switch(tp->autoHide) {
   case THIDE_LEFT:
      x = sp->y - tp->width - TRAY_BORDER_SIZE;
      y = tp->y;
      break;
   case THIDE_RIGHT:
      x = sp->y + sp->width - TRAY_BORDER_SIZE;
      y = tp->y;
      break;
   case THIDE_TOP:
      x = tp->x;
      y = sp->y - tp->height - TRAY_BORDER_SIZE;
      break;
   case THIDE_BOTTOM:
      x = tp->x;
      y = sp->y + sp->height - TRAY_BORDER_SIZE;
      break;
   default:
      Assert(0);
      break;
   }

   /* Move and redraw. */
   JXMoveWindow(display, tp->window, x, y);
   DrawSpecificTray(tp);

}

/** Process a tray event. */
char ProcessTrayEvent(const XEvent *event)
{

   TrayType *tp;

   for(tp = trays; tp; tp = tp->next) {
      if(event->xany.window == tp->window) {
         switch(event->type) {
         case Expose:
            HandleTrayExpose(tp, &event->xexpose);
            return 1;
         case EnterNotify:
            HandleTrayEnterNotify(tp, &event->xcrossing);
            return 1;
         case ButtonPress:
            HandleTrayButtonPress(tp, &event->xbutton);
            return 1;
         case ButtonRelease:
            HandleTrayButtonRelease(tp, &event->xbutton);
            return 1;
         case MotionNotify:
            HandleTrayMotionNotify(tp, &event->xmotion);
            return 1;
         default:
            return 0;
         }
      }
   }

   return 0;

}

/** Signal the tray (needed for autohide). */
void SignalTray(const TimeType *now, int x, int y, Window w, void *data)
{
   TrayType *tp = (TrayType*)data;
   if(tp->autoHide != THIDE_OFF && !tp->hidden && !menuShown) {
      if(x < tp->x || x >= tp->x + tp->width
         || y < tp->y || y >= tp->y + tp->height) {
         HideTray(tp);
      }
   }
}

/** Handle a tray expose event. */
void HandleTrayExpose(TrayType *tp, const XExposeEvent *event)
{
   DrawSpecificTray(tp);
}

/** Handle a tray enter notify (for autohide). */
void HandleTrayEnterNotify(TrayType *tp, const XCrossingEvent *event)
{
   ShowTray(tp);
}

/** Get the tray component under the given coordinates. */
TrayComponentType *GetTrayComponent(TrayType *tp, int x, int y)
{

   TrayComponentType *cp;
   int xoffset, yoffset;

   xoffset = 0;
   yoffset = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      int startx = xoffset;
      int starty = yoffset;
      int width = cp->width;
      int height = cp->height;
      if(x >= startx && x < startx + width) {
         if(y >= starty && y < starty + height) {
            return cp;
         }
      }
      if(tp->layout == LAYOUT_HORIZONTAL) {
         xoffset += cp->width;
      } else {
         yoffset += cp->height;
      }
   }

   return NULL;

}

/** Handle a button press on a tray. */
void HandleTrayButtonPress(TrayType *tp, const XButtonEvent *event)
{
   TrayComponentType *cp = GetTrayComponent(tp, event->x, event->y);
   if(cp && cp->ProcessButtonPress) {
      const int x = event->x - cp->x;
      const int y = event->y - cp->y;
      const int mask = event->button;
      (cp->ProcessButtonPress)(cp, x, y, mask);
   }
}

/** Handle a button release on a tray. */
void HandleTrayButtonRelease(TrayType *tp, const XButtonEvent *event)
{

   TrayComponentType *cp;

   // First inform any components that have a grab.
   for(cp = tp->components; cp; cp = cp->next) {
      if(cp->grabbed) {
         const int x = event->x - cp->x;
         const int y = event->y - cp->y;
         const int mask = event->button;
         (cp->ProcessButtonRelease)(cp, x, y, mask);
         JXUngrabPointer(display, CurrentTime);
         cp->grabbed = 0;
         return;
      }
   }

   cp = GetTrayComponent(tp, event->x, event->y);
   if(cp && cp->ProcessButtonRelease) {
      const int x = event->x - cp->x;
      const int y = event->y - cp->y;
      const int mask = event->button;
      (cp->ProcessButtonRelease)(cp, x, y, mask);
   }

}

/** Handle a motion notify event. */
void HandleTrayMotionNotify(TrayType *tp, const XMotionEvent *event)
{

   TrayComponentType *cp = GetTrayComponent(tp, event->x, event->y);
   if(cp && cp->ProcessMotionEvent) {
      const int x = event->x - cp->x;
      const int y = event->y - cp->y;
      const int mask = event->state;
      (cp->ProcessMotionEvent)(cp, x, y, mask);
   }

}

/** Draw all trays. */
void DrawTray(void)
{

   TrayType *tp;

   if(shouldExit) {
      return;
   }

   for(tp = trays; tp; tp = tp->next) {
      DrawSpecificTray(tp);
   }

}

/** Draw a specific tray. */
void DrawSpecificTray(const TrayType *tp)
{

   TrayComponentType *cp;

   for(cp = tp->components; cp; cp = cp->next) {
      UpdateSpecificTray(tp, cp);
   }

}

/** Raise tray windows. */
void RaiseTrays(void)
{
   TrayType *tp;
   for(tp = trays; tp; tp = tp->next) {
      tp->autoHide |= THIDE_RAISED;
      ShowTray(tp);
      JXRaiseWindow(display, tp->window);
   }
}

/** Lower tray windows. */
void LowerTrays(void)
{
   TrayType *tp;
   for(tp = trays; tp; tp = tp->next) {
      tp->autoHide &= ~THIDE_RAISED;
   }
   RestackClients();
}

/** Update a specific component on a tray. */
void UpdateSpecificTray(const TrayType *tp, const TrayComponentType *cp)
{

   if(JUNLIKELY(shouldExit)) {
      return;
   }

   /* If the tray is hidden, draw only the background. */
   if(!tp->hidden && cp->pixmap != None) {
      JXCopyArea(display, cp->pixmap, tp->window, rootGC, 0, 0,
                 cp->width, cp->height, cp->x, cp->y);
   }

}

/** Layout tray components on a tray. */
void LayoutTray(TrayType *tp, int *variableSize, int *variableRemainder)
{

   TrayComponentType *cp;
   unsigned int variableCount;
   int width, height;
   int temp;

   tp->width = tp->requestedWidth;
   tp->height = tp->requestedHeight;

   for(cp = tp->components; cp; cp = cp->next) {
      cp->width = cp->requestedWidth;
      cp->height = cp->requestedHeight;
   }

   ComputeTraySize(tp);

   /* Get the remaining size after setting fixed size components. */
   /* Also, keep track of the number of variable size components. */
   width = tp->width;
   height = tp->height;
   variableCount = 0;
   for(cp = tp->components; cp; cp = cp->next) {
      if(tp->layout == LAYOUT_HORIZONTAL) {
         temp = cp->width;
         if(temp > 0) {
            width -= temp;
         } else {
            variableCount += 1;
         }
      } else {
         temp = cp->height;
         if(temp > 0) {
            height -= temp;
         } else {
            variableCount += 1;
         }
      }
   }

   /* Distribute excess size among variable size components.
    * If there are no variable size components, shrink the tray.
    * If we are out of room, just give them a size of one.
    */
   *variableSize = 1;
   *variableRemainder = 0;
   if(tp->layout == LAYOUT_HORIZONTAL) {
      if(variableCount) {
         if(width >= variableCount) {
            *variableSize = width / variableCount;
            *variableRemainder = width % variableCount;
         }
      } else if(width > 0) {
         tp->width -= width;
      }
   } else {
      if(variableCount) {
         if(height >= variableCount) {
            *variableSize = height / variableCount;
            *variableRemainder = height % variableCount;
         }
      } else if(height > 0) {
         tp->height -= height;
      }
   }

   tp->width = Max(1, tp->width);
   tp->height = Max(1, tp->height);

}

/** Resize a tray. */
void ResizeTray(TrayType *tp)
{

   TrayComponentType *cp;
   int variableSize;
   int variableRemainder;
   int xoffset, yoffset;
   int width, height;

   Assert(tp);

   LayoutTray(tp, &variableSize, &variableRemainder);

   /* Reposition items on the tray. */
   xoffset = 0;
   yoffset = 0;
   for(cp = tp->components; cp; cp = cp->next) {

      cp->x = xoffset;
      cp->y = yoffset;
      cp->screenx = tp->x + xoffset;
      cp->screeny = tp->y + yoffset;

      if(cp->Resize) {
         if(tp->layout == LAYOUT_HORIZONTAL) {
            height = tp->height;
            width = cp->width;
            if(width == 0) {
               width = variableSize;
               if(variableRemainder) {
                  width += 1;
                  variableRemainder -= 1;
               }
            }
         } else {
            width = tp->width;
            height = cp->height;
            if(height == 0) {
               height = variableSize;
               if(variableRemainder) {
                  height += 1;
                  variableRemainder -= 1;
               }
            }
         }
         cp->width = width;
         cp->height = height;
         (cp->Resize)(cp);
      }

      if(cp->window != None) {
         JXMoveWindow(display, cp->window, xoffset, yoffset);
      }

      if(tp->layout == LAYOUT_HORIZONTAL) {
         xoffset += cp->width;
      } else {
         yoffset += cp->height;
      }
   }

   JXMoveResizeWindow(display, tp->window, tp->x, tp->y,
                      tp->width, tp->height);

   UpdateTaskBar();
   DrawSpecificTray(tp);

   if(tp->hidden) {
      HideTray(tp);
   }

}

/** Draw the tray background on a drawable. */
void ClearTrayDrawable(const TrayComponentType *cp)
{
   const Drawable d = cp->pixmap != None ? cp->pixmap : cp->window;
   if(colors[COLOR_TRAY_BG1] == colors[COLOR_TRAY_BG2]) {
      JXSetForeground(display, rootGC, colors[COLOR_TRAY_BG1]);
      JXFillRectangle(display, d, rootGC, 0, 0, cp->width, cp->height);
   } else {
      DrawHorizontalGradient(d, rootGC, colors[COLOR_TRAY_BG1],
                             colors[COLOR_TRAY_BG2], 0, 0,
                             cp->width, cp->height);
   }
}

/** Get a linked list of trays. */
TrayType *GetTrays(void)
{
   return trays;
}

/** Get the number of trays. */
unsigned int GetTrayCount(void)
{
   return trayCount;
}

/** Determine if a tray should autohide. */
void SetAutoHideTray(TrayType *tp, TrayAutoHideType autohide)
{
   Assert(tp);
   tp->autoHide = autohide;
}

/** Set the x-coordinate of a tray. */
void SetTrayX(TrayType *tp, const char *str)
{
   Assert(tp);
   Assert(str);
   tp->requestedX = atoi(str);
}

/** Set the y-coordinate of a tray. */
void SetTrayY(TrayType *tp, const char *str)
{
   Assert(tp);
   Assert(str);
   tp->requestedY = atoi(str);
}

/** Set the width of a tray. */
void SetTrayWidth(TrayType *tp, const char *str)
{

   int width;

   Assert(tp);
   Assert(str);

   width = atoi(str);

   if(JUNLIKELY(width < 0)) {
      Warning(_("invalid tray width: %d"), width);
   } else {
      tp->requestedWidth = width;
   }

}

/** Set the height of a tray. */
void SetTrayHeight(TrayType *tp, const char *str)
{

   int height;

   Assert(tp);
   Assert(str);

   height = atoi(str);

   if(JUNLIKELY(height < 0)) {
      Warning(_("invalid tray height: %d"), height);
   } else {
      tp->requestedHeight = height;
   }

}


/** Set the tray orientation. */
void SetTrayLayout(TrayType *tp, const char *str)
{

   Assert(tp);

   if(!str) {

      /* Compute based on requested size. */

   } else if(!strcmp(str, "horizontal")) {

      tp->layout = LAYOUT_HORIZONTAL;
      return;

   } else if(!strcmp(str, "vertical")) {

      tp->layout = LAYOUT_VERTICAL;
      return;

   } else {
      Warning(_("invalid tray layout: \"%s\""), str);
   }

   /* Prefer horizontal layout, but use vertical if
    * width is finite and height is larger than width or infinite.
    */
   if(tp->requestedWidth > 0
      && (tp->requestedHeight == 0
      || tp->requestedHeight > tp->requestedWidth)) {
      tp->layout = LAYOUT_VERTICAL;
   } else {
      tp->layout = LAYOUT_HORIZONTAL;
   }

}

/** Set the layer for a tray. */
void SetTrayLayer(TrayType *tp, WinLayerType layer)
{
   tp->layer = layer;
}

/** Set the horizontal tray alignment. */
void SetTrayHorizontalAlignment(TrayType *tp, const char *str)
{
   static const StringMappingType mapping[] = {
      { "center",    TALIGN_CENTER  },
      { "fixed",     TALIGN_FIXED   },
      { "left",      TALIGN_LEFT    },
      { "right",     TALIGN_RIGHT   }
   };

   if(!str) {
      tp->halign = TALIGN_FIXED;
   } else {
      const int x = FindValue(mapping, ARRAY_LENGTH(mapping), str);
      if(JLIKELY(x >= 0)) {
         tp->halign = x;
      } else {
         Warning(_("invalid tray horizontal alignment: \"%s\""), str);
         tp->halign = TALIGN_FIXED;
      }
   }
}

/** Set the vertical tray alignment. */
void SetTrayVerticalAlignment(TrayType *tp, const char *str)
{
   static const StringMappingType mapping[] = {
      { "bottom",    TALIGN_BOTTOM  },
      { "center",    TALIGN_CENTER  },
      { "fixed",     TALIGN_FIXED   },
      { "top",       TALIGN_TOP     }
   };

   if(!str) {
      tp->valign = TALIGN_FIXED;
   } else {
      const int x = FindValue(mapping, ARRAY_LENGTH(mapping), str);
      if(JLIKELY(x >= 0)) {
         tp->valign = x;
      } else {
         Warning(_("invalid tray vertical alignment: \"%s\""), str);
         tp->valign = TALIGN_FIXED;
      }
   }
}
