#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "globals.hpp"
#include "BarPassElement.hpp"
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include "appName.hpp"
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <algorithm>
#include <climits>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) { return CHyprColor{static_cast<uint64_t>(color)}; }

static std::string windowCommand(std::string command, const PHLWINDOW &window) {
  // Resolve the decoration owner synchronously. An asynchronously spawned
  // command must not choose its target from whatever happens to be focused.
  constexpr std::string_view token = "%WINDOW%";
  const auto address = std::format("0x{:x}", reinterpret_cast<uintptr_t>(window.get()));
  size_t offset = 0;
  while ((offset = command.find(token, offset)) != std::string::npos) {
    command.replace(offset, token.size(), address);
    offset += address.size();
  }
  return command;
}

COmarchyFloatingHyprBar::COmarchyFloatingHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
  m_pWindow = pWindow;

  const auto PMONITOR = pWindow->m_monitor.lock();
  PMONITOR->m_scheduledRecalc = true;

  // button events
  m_pMouseButtonCallback =
      Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo &info) { onMouseButton(info, e); });
  m_pTouchDownCallback = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo &info) { onTouchDown(info, e); });
  m_pTouchUpCallback = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo &info) { onTouchUp(info, e); });

  // move events
  m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo &info) { onTouchMove(info, e); });
  m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo &info) { onMouseMove(c); });

  Animation::mgr()->createAnimation(configColor(g_pOmarchyFloatingGlobalState->config.barColor->value()), m_cRealBarColor,
                                    Config::animationTree()->getAnimationPropertyConfig("border"), pWindow, AVARDAMAGE_NONE);
  m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });
}

COmarchyFloatingHyprBar::~COmarchyFloatingHyprBar() { std::erase(g_pOmarchyFloatingGlobalState->bars, m_self); }

SDecorationPositioningInfo COmarchyFloatingHyprBar::getPositioningInfo() {
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto ENABLED = g_pOmarchyFloatingGlobalState->config.enabled->value();
  const auto PRECEDENCE = g_pOmarchyFloatingGlobalState->config.barPrecedenceOverBorder->value();

  SDecorationPositioningInfo info;
  info.policy = m_hidden ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
  info.edges = DECORATION_EDGE_TOP;
  info.priority = PRECEDENCE ? 10005 : 5000;
  info.reserved = true;
  info.desiredExtents = {{0, m_hidden || !ENABLED ? 0 : HEIGHT}, {0, 0}};
  return info;
}

void COmarchyFloatingHyprBar::onPositioningReply(const SDecorationPositioningReply &reply) {
  if (reply.assignedGeometry.size() != m_bAssignedBox.size())
    m_bWindowSizeChanged = true;

  m_bAssignedBox = reply.assignedGeometry;
}

std::string COmarchyFloatingHyprBar::getDisplayName() { return "Hyprbar"; }

bool COmarchyFloatingHyprBar::inputIsValid() {
  if (m_hidden || !g_pOmarchyFloatingGlobalState->config.enabled->value())
    return false;

  if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
    return false;

  const auto MOUSE = g_pInputManager->getMouseCoordsInternal();
  auto PMONITOR = Desktop::focusState()->monitor();

  if (!PMONITOR)
    return false;

  Desktop::CViewHitTester hitTester{*Desktop::viewState()};

  const auto WINDOWATCURSOR = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

  auto focusState = Desktop::focusState();
  auto window = focusState->window();

  // The focused window's bar can sit under another window, and a press there
  // belongs to that window.
  if (WINDOWATCURSOR != m_pWindow && (m_pWindow != window || WINDOWATCURSOR))
    return false;

  PHLLS foundSurface = nullptr;
  Vector2D surfaceCoords;

  // Check Top Layer
  hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
  if (foundSurface)
    return false;

  // Check Overlay Layer
  hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);
  if (foundSurface)
    return false;

  return true;
}

void COmarchyFloatingHyprBar::onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e) {
  if (e.button != BTN_LEFT)
    return;

  if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
    // A titlebar drag can finish over a panel or another surface.
    if (m_bDraggingThis || m_bCancelledDown || m_bDragPending)
      handleUpEvent(info);
    return;
  }

  if (!inputIsValid())
    return;

  handleDownEvent(info, std::nullopt);
}

void COmarchyFloatingHyprBar::onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e) {
  // Don't do anything if you're already grabbed a window with another finger
  if (!inputIsValid() || e.touchID != 0)
    return;

  handleDownEvent(info, e);
}

void COmarchyFloatingHyprBar::onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e) {
  // A tap on a button, or a double tap, leaves no drag pending, and its press
  // was taken all the same: left armed, the bar would swallow the release of
  // the next mouse click in the window.
  if (!m_bTouchEv || e.touchID != m_touchId || !(m_bDragPending || m_bCancelledDown || m_bDraggingThis))
    return;

  handleUpEvent(info);
}

void COmarchyFloatingHyprBar::onMouseMove(Vector2D coords) {
  // Hovered buttons are highlighted, so redraw when the pointer moves between
  // them, also with hardware cursors.
  damageOnButtonHover();

  if (m_bDraggingThis && !m_bTouchEv)
    updateSnap();

  if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
    return;

  m_bDragPending = false;
  handleMovement();
}

void COmarchyFloatingHyprBar::onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e) {
  if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
    return;

  if (!m_bDraggingThis) {
    // Initial setup for dragging a window.
    (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, m_pWindow.lock());
    // Pin it so you can change workspaces while dragging a window
    (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, m_pWindow.lock());

    g_layoutManager->beginDragTarget(m_pWindow.lock()->layoutTarget(), MBIND_MOVE);
    m_bDraggingThis = true;
  }
}

void COmarchyFloatingHyprBar::handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent) {
  m_bTouchEv = touchEvent.has_value();
  if (m_bTouchEv)
    m_touchId = touchEvent.value().touchID;

  const auto PWINDOW = m_pWindow.lock();

  auto COORDS = cursorRelativeToBar();
  if (m_bTouchEv) {
    ITouch::SDownEvent e = touchEvent.value();
    PHLMONITOR PMONITOR = nullptr;
    for (auto &m : State::monitorState()->monitors()) {
      if (m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
        PMONITOR = m;
        break;
      }
    }
    PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - assignedBoxGlobal().pos();
  }

  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ON_DOUBLE_CLICK = g_pOmarchyFloatingGlobalState->config.onDoubleClick->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

  // Hyprland measures its border resize band from the window's content, so for
  // a titled window the band sits under the bar and the edge above the bar
  // never resizes. A press in the band above the bar starts that resize here.
  if (!m_bTouchEv && startTopEdgeResize(info, PWINDOW, COORDS))
    return;

  if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, HEIGHT - 1)) {

    if (m_bDraggingThis) {
      if (m_bTouchEv)
        (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);
      g_layoutManager->endDragTarget();
      LOG(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
    }

    m_bDraggingThis = false;
    m_bDragPending = false;
    m_bTouchEv = false;
    return;
  }

  if (Desktop::focusState()->window() != PWINDOW)
    Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

  if (PWINDOW->isFloating())
    Desktop::windowState()->raise(PWINDOW);

  info.cancelled = true;
  m_bCancelledDown = true;

  if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS, BUTTONSRIGHT))
    return;

  if (!ON_DOUBLE_CLICK.empty() &&
      std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
    Config::Supplementary::executor()->spawn(windowCommand(ON_DOUBLE_CLICK, PWINDOW));
    m_bDragPending = false;
  } else {
    m_lastMouseDown = Time::steadyNow();
    m_bDragPending = true;
  }
}

bool COmarchyFloatingHyprBar::startTopEdgeResize(Event::SCallbackInfo &info, PHLWINDOW window, const Vector2D &coords) {
  static const auto RESIZEONBORDER = CConfigValue<Config::INTEGER>("general:resize_on_border");
  static const auto EXTENDGRAB = CConfigValue<Config::INTEGER>("general:extend_border_grab_area");

  if (!*RESIZEONBORDER || !window || !window->isFloating() || Fullscreen::controller()->isFullscreen(window))
    return false;

  // The band runs from the top of the bar out over the border and the grab
  // area beyond it, across the bar's width and the grab area at its corners.
  const double GRAB = window->presentation().borderSize() + *EXTENDGRAB;
  const double WIDTH = assignedBoxGlobal().w;
  if (coords.y >= 0 || coords.y < -GRAB || coords.x < -GRAB || coords.x >= WIDTH + GRAB)
    return false;

  if (Desktop::focusState()->window() != window)
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
  Desktop::windowState()->raise(window);

  g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_RESIZE);

  // The press never reaches Hyprland, so neither does the release: the up
  // handler ends the resize the way it ends a titlebar drag.
  info.cancelled = true;
  m_bCancelledDown = true;
  m_bDraggingThis = true;
  m_bDragPending = false;
  return true;
}

void COmarchyFloatingHyprBar::handleUpEvent(Event::SCallbackInfo &info) {
  // Only the bar that took the press gets here, and its window can have lost
  // focus by the release, as one minimized from its own button has. The
  // release is still this bar's: left armed, it would swallow the release of
  // a later click in the window, and the app would see the button held down.
  if (m_bCancelledDown)
    info.cancelled = true;

  m_bCancelledDown = false;

  if (m_bDraggingThis) {
    const bool MOVING = !m_bTouchEv && g_layoutManager->dragController()->mode() == MBIND_MOVE;
    g_layoutManager->endDragTarget();
    m_bDraggingThis = false;
    if (m_bTouchEv)
      (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);
    else if (MOVING && m_snapZone != SNAP_NONE)
      // After the drag has ended, or ending it would put the window back.
      applySnap();
    else if (MOVING)
      keepTitlebarReachable();

    LOG(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
  }

  clearSnap();
  m_bDragPending = false;
  m_bTouchEv = false;
  m_touchId = 0;
}

void COmarchyFloatingHyprBar::handleMovement() {
  const auto window = m_pWindow.lock();
  const auto cursor = g_pInputManager->getMouseCoordsInternal();
  const auto bar = assignedBoxGlobal();
  const auto anchor = std::clamp((cursor.x - bar.x) / std::max(1.0, bar.w), 0.0, 1.0);
  const auto offset = std::clamp(cursor.y - bar.y, 0.0, bar.h);
  const bool maximized = Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_MAXIMIZED);
  if (maximized)
    Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);

  // Where the drag comes from: the tile it leaves (for a trade on release) and
  // its own box, given back after a later snap. A snapped window gets its own
  // size back under the pointer first.
  m_dragFromZone = maximized ? SNAP_NONE : snappedZone();
  if (!maximized)
    restoreSnappedSize(cursor, anchor, offset);
  m_dragFromBox = window->layoutTarget()->position();

  if (maximized) {
    auto box = window->layoutTarget()->position();
    const auto reserved = window->getFullWindowReservedArea();
    box.x = cursor.x - box.w * anchor;
    box.y = cursor.y + reserved.topLeft.y - offset;
    g_layoutManager->setTargetGeom(box, window->layoutTarget());
    window->layoutTarget()->warpPositionSize();
  }
  g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_MOVE);
  m_bDraggingThis = true;
  LOG(Log::DEBUG, "[hyprbars] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
  return;
}

void COmarchyFloatingHyprBar::keepTitlebarReachable() {
  // A window dropped with its titlebar under a top bar or off the screen
  // can't be dragged back, so keep the reserved top (border and titlebar)
  // between the top and bottom of the work area.
  const auto window = m_pWindow.lock();
  if (!validMapped(window) || !window->isFloating())
    return;

  auto monitor = State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run();
  monitor = monitor ? monitor : window->m_monitor.lock();
  if (!monitor)
    return;

  const auto work = monitor->logicalBoxMinusReserved();
  const auto reserved = window->getFullWindowReservedArea();
  auto box = window->layoutTarget()->position();
  const double top = box.y - reserved.topLeft.y;
  const double reachable = std::clamp(top, work.y, std::max(work.y, work.y + work.h - reserved.topLeft.y));
  if (std::abs(reachable - top) < 0.5)
    return;

  box.y = reachable + reserved.topLeft.y;
  g_layoutManager->setTargetGeom(box, window->layoutTarget());
  LOG(Log::DEBUG, "[hyprbars] Moved {:x} to keep its titlebar reachable", (uintptr_t)window.get());
}

bool COmarchyFloatingHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS,
                                            const bool BUTTONSRIGHT) {
  // check if on a button
  float offset = barPadding;

  for (auto &b : g_pOmarchyFloatingGlobalState->buttons) {
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, barHeight};
    Vector2D currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - barButtonPadding - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

    if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + barButtonPadding, currentPos.y + b.size)) {
      // hit on close
      Config::Supplementary::executor()->spawn(windowCommand(b.cmd, m_pWindow.lock()));
      return true;
    }

    offset += barButtonPadding + b.size;
  }
  return false;
}

// ---- Edge snapping ----------------------------------------------------------
// The pointer has to reach the screen's edge (it stops there), and a corner
// reaches CORNER of the side along each edge, so a corner is easy to hit.
static constexpr double SNAP_EDGE = 2.0;
static constexpr double SNAP_CORNER = 0.2;

// The boxes a zone gives this window on a monitor: the whole window as drawn
// (preview) and the window box itself (target). Laid out as tiling would:
// gaps_out to the work area, gaps_in between halves, the window box inside its
// border and below this titlebar.
void COmarchyFloatingHyprBar::zoneBoxes(eSnapZone zone, PHLMONITOR monitor, CBox &preview, CBox &target) {
  const auto window = m_pWindow.lock();
  static auto PGAPSIN = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
  static auto PGAPSOUT = CConfigValue<Config::IComplexConfigValue>("general:gaps_out");
  const auto *GAPSIN = sc<Config::CCssGapData *>(PGAPSIN.ptr());
  const auto *GAPSOUT = sc<Config::CCssGapData *>(PGAPSOUT.ptr());

  const auto area = monitor->logicalBoxMinusReserved();
  double l = area.x + GAPSOUT->m_left, t = area.y + GAPSOUT->m_top;
  double r = area.x + area.w - GAPSOUT->m_right, b = area.y + area.h - GAPSOUT->m_bottom;
  const double MIDX = area.x + area.w / 2, MIDY = area.y + area.h / 2;

  if (zone == SNAP_LEFT || zone == SNAP_TOP_LEFT || zone == SNAP_BOTTOM_LEFT)
    r = MIDX - GAPSIN->m_right;
  if (zone == SNAP_RIGHT || zone == SNAP_TOP_RIGHT || zone == SNAP_BOTTOM_RIGHT)
    l = MIDX + GAPSIN->m_left;
  if (zone == SNAP_TOP_LEFT || zone == SNAP_TOP_RIGHT)
    b = MIDY - GAPSIN->m_bottom;
  if (zone == SNAP_BOTTOM_LEFT || zone == SNAP_BOTTOM_RIGHT)
    t = MIDY + GAPSIN->m_top;

  preview = CBox{l, t, r - l, b - t}.round();

  const double BORDER = window->presentation().borderSize();
  const auto RESERVED = window->getFullWindowReservedArea();
  target = CBox{l + BORDER + RESERVED.topLeft.x, t + BORDER + RESERVED.topLeft.y,
                r - l - 2 * BORDER - RESERVED.topLeft.x - RESERVED.bottomRight.x,
                b - t - 2 * BORDER - RESERVED.topLeft.y - RESERVED.bottomRight.y}
               .round();
}

void COmarchyFloatingHyprBar::updateSnap() {
  const auto window = m_pWindow.lock();
  const auto monitor = Desktop::focusState()->monitor();
  if (!g_pOmarchyFloatingGlobalState->config.edgeSnap->value() || !window || !window->isFloating() || !monitor) {
    clearSnap();
    return;
  }

  const auto cursor = g_pInputManager->getMouseCoordsInternal();
  const auto screen = monitor->logicalBox();
  const bool left = cursor.x <= screen.x + SNAP_EDGE;
  const bool right = cursor.x >= screen.x + screen.w - 1 - SNAP_EDGE;
  const bool top = cursor.y <= screen.y + SNAP_EDGE;
  const bool bottom = cursor.y >= screen.y + screen.h - 1 - SNAP_EDGE;
  const bool upper = cursor.y < screen.y + screen.h * SNAP_CORNER;
  const bool lower = cursor.y > screen.y + screen.h * (1 - SNAP_CORNER);
  const bool west = cursor.x < screen.x + screen.w * SNAP_CORNER;
  const bool east = cursor.x > screen.x + screen.w * (1 - SNAP_CORNER);

  eSnapZone zone = SNAP_NONE;
  if ((left && upper) || (top && west))
    zone = SNAP_TOP_LEFT;
  else if ((right && upper) || (top && east))
    zone = SNAP_TOP_RIGHT;
  else if ((left && lower) || (bottom && west))
    zone = SNAP_BOTTOM_LEFT;
  else if ((right && lower) || (bottom && east))
    zone = SNAP_BOTTOM_RIGHT;
  else if (left)
    zone = SNAP_LEFT;
  else if (right)
    zone = SNAP_RIGHT;
  else if (top)
    zone = SNAP_TOP;

  if (zone == m_snapZone && monitor == m_snapMonitor.lock())
    return;

  if (m_snapZone != SNAP_NONE)
    g_pHyprRenderer->damageBox(CBox{m_snapPreview}.expand(2));

  m_snapZone = zone;
  m_snapMonitor = monitor;
  if (zone == SNAP_NONE)
    return;

  zoneBoxes(zone, monitor, m_snapPreview, m_snapTarget);

  g_pHyprRenderer->damageBox(CBox{m_snapPreview}.expand(2));
}

void COmarchyFloatingHyprBar::clearSnap() {
  if (m_snapZone != SNAP_NONE)
    g_pHyprRenderer->damageBox(CBox{m_snapPreview}.expand(2));
  m_snapZone = SNAP_NONE;
  m_snapMonitor.reset();
}

void COmarchyFloatingHyprBar::applySnap() {
  const auto window = m_pWindow.lock();
  if (m_snapZone == SNAP_NONE || !window || !window->isFloating() || m_snapTarget.w < 1 || m_snapTarget.h < 1)
    return;

  if (const auto monitor = m_snapMonitor.lock())
    makeRoom(m_snapZone, m_dragFromZone, monitor);
  // The window's own box is the one from before the drag, not the one it was
  // dropped with, which hangs off the screen edge under the pointer.
  m_preSnapBox = m_dragFromBox;
  g_layoutManager->setTargetGeom(m_snapTarget, window->layoutTarget());
}

// The tile the window sits in, worked out from its box (within a couple of
// pixels), or SNAP_NONE. Nothing else is remembered, so this survives reloads,
// and a window moved or resized some other way simply isn't snapped any more.
COmarchyFloatingHyprBar::eSnapZone COmarchyFloatingHyprBar::snappedZone() {
  const auto window = m_pWindow.lock();
  const auto monitor = window ? window->m_monitor.lock() : nullptr;
  if (!validMapped(window) || !window->isFloating() || !monitor)
    return SNAP_NONE;

  const auto box = window->layoutTarget()->position();
  for (const auto zone : {SNAP_LEFT, SNAP_RIGHT, SNAP_TOP, SNAP_TOP_LEFT, SNAP_TOP_RIGHT, SNAP_BOTTOM_LEFT, SNAP_BOTTOM_RIGHT}) {
    CBox preview, target;
    zoneBoxes(zone, monitor, preview, target);
    if (std::abs(box.x - target.x) <= 2 && std::abs(box.y - target.y) <= 2 && std::abs(box.w - target.w) <= 2 && std::abs(box.h - target.h) <= 2)
      return zone;
  }
  return SNAP_NONE;
}

void COmarchyFloatingHyprBar::snapTo(eSnapZone zone, const CBox &target) {
  const auto window = m_pWindow.lock();
  // Keep the box from before the first snap, so moving between zones and then
  // out again gives back the window's own size (and, by key, its place).
  if (snappedZone() == SNAP_NONE)
    m_preSnapBox = window->layoutTarget()->position();
  g_layoutManager->setTargetGeom(target, window->layoutTarget());
}

// The window's own box, kept inside the work area of the monitor it is on now.
// Without one (the plugin was reloaded since the snap), Omarchy's size for a
// newly floated window, centred.
CBox COmarchyFloatingHyprBar::ownBox(PHLMONITOR monitor) {
  const auto area = monitor->logicalBoxMinusReserved();
  CBox box = m_preSnapBox.value_or(CBox{0, 0, std::min(1100.0, area.w * 0.64), std::min(760.0, area.h * 0.68)});
  box.w = std::min(box.w, area.w);
  box.h = std::min(box.h, area.h);
  if (!m_preSnapBox) {
    box.x = area.x + (area.w - box.w) / 2;
    box.y = area.y + (area.h - box.h) / 2;
  }
  box.x = std::clamp(box.x, area.x, area.x + area.w - box.w);
  box.y = std::clamp(box.y, area.y, area.y + area.h - box.h);
  return box;
}

void COmarchyFloatingHyprBar::restoreSnapped() {
  const auto window = m_pWindow.lock();
  const auto monitor = window ? window->m_monitor.lock() : nullptr;
  if (!window || !monitor)
    return;
  g_layoutManager->setTargetGeom(ownBox(monitor), window->layoutTarget());
  m_preSnapBox.reset();
}

// Before this window takes `zone`, move the snapped windows in its way on the
// same workspace: one in that very tile trades places with it (when it comes
// from a tile of its own), as a swap does; one holding the half a quarter is cut
// from keeps the other quarter of that half, so the two share it.
void COmarchyFloatingHyprBar::makeRoom(eSnapZone zone, eSnapZone from, PHLMONITOR monitor) {
  const auto window = m_pWindow.lock();
  auto halfOf = [](eSnapZone z) {
    if (z == SNAP_TOP_LEFT || z == SNAP_BOTTOM_LEFT)
      return SNAP_LEFT;
    if (z == SNAP_TOP_RIGHT || z == SNAP_BOTTOM_RIGHT)
      return SNAP_RIGHT;
    return SNAP_NONE;
  };
  auto otherQuarter = [](eSnapZone z) {
    switch (z) {
      case SNAP_TOP_LEFT: return SNAP_BOTTOM_LEFT;
      case SNAP_BOTTOM_LEFT: return SNAP_TOP_LEFT;
      case SNAP_TOP_RIGHT: return SNAP_BOTTOM_RIGHT;
      case SNAP_BOTTOM_RIGHT: return SNAP_TOP_RIGHT;
      default: return SNAP_NONE;
    }
  };

  // Decorations own their bars, so these weak pointers are only read, never locked.
  for (const auto &weak : g_pOmarchyFloatingGlobalState->bars) {
    const auto other = weak.get();
    if (!other || other == this)
      continue;
    const auto otherWindow = other->getOwner();
    if (!validMapped(otherWindow) || otherWindow->m_workspace != window->m_workspace)
      continue;

    const auto held = other->snappedZone();
    eSnapZone moveTo = SNAP_NONE;
    if (held == zone && from != SNAP_NONE)
      moveTo = from;
    else if (held != SNAP_NONE && held == halfOf(zone))
      moveTo = otherQuarter(zone);
    if (moveTo == SNAP_NONE)
      continue;

    CBox preview, target;
    other->zoneBoxes(moveTo, monitor, preview, target);
    other->snapTo(moveTo, target);
  }
}

// Super + Shift + arrow on a floating workspace: move the window one step that
// way between halves and quarters, as the same keys swap it on a tiling one.
// Up builds up to the whole work area, Down takes it back to its own box.
bool COmarchyFloatingHyprBar::keySnap(char dir) {
  const auto window = m_pWindow.lock();
  if (!g_pOmarchyFloatingGlobalState->config.edgeSnap->value() || !validMapped(window) || !window->isFloating() || m_hidden ||
      Fullscreen::controller()->isFullscreen(window))
    return false;
  const auto monitor = window->m_monitor.lock();
  if (!monitor)
    return false;

  enum : uint8_t { STAY = 0xfe, RESTORE = 0xff };
  const auto from = snappedZone();
  uint8_t to = STAY;
  switch (from) {
    case SNAP_NONE: to = dir == 'l' ? SNAP_LEFT : dir == 'r' ? SNAP_RIGHT : dir == 'u' ? SNAP_TOP : STAY; break;
    case SNAP_LEFT: to = dir == 'r' ? SNAP_RIGHT : dir == 'u' ? SNAP_TOP_LEFT : dir == 'd' ? SNAP_BOTTOM_LEFT : STAY; break;
    case SNAP_RIGHT: to = dir == 'l' ? SNAP_LEFT : dir == 'u' ? SNAP_TOP_RIGHT : dir == 'd' ? SNAP_BOTTOM_RIGHT : STAY; break;
    case SNAP_TOP_LEFT: to = dir == 'r' ? SNAP_TOP_RIGHT : dir == 'u' ? SNAP_TOP : dir == 'd' ? SNAP_LEFT : STAY; break;
    case SNAP_TOP_RIGHT: to = dir == 'l' ? SNAP_TOP_LEFT : dir == 'u' ? SNAP_TOP : dir == 'd' ? SNAP_RIGHT : STAY; break;
    case SNAP_BOTTOM_LEFT: to = dir == 'r' ? SNAP_BOTTOM_RIGHT : dir == 'u' ? SNAP_LEFT : dir == 'd' ? RESTORE : STAY; break;
    case SNAP_BOTTOM_RIGHT: to = dir == 'l' ? SNAP_BOTTOM_LEFT : dir == 'u' ? SNAP_RIGHT : dir == 'd' ? RESTORE : STAY; break;
    case SNAP_TOP: to = dir == 'l' ? SNAP_LEFT : dir == 'r' ? SNAP_RIGHT : dir == 'd' ? RESTORE : STAY; break;
  }

  if (to == STAY)
    return true;
  if (to == RESTORE) {
    restoreSnapped();
    return true;
  }

  const auto zone = static_cast<eSnapZone>(to);

  makeRoom(zone, from, monitor);

  CBox preview, target;
  zoneBoxes(zone, monitor, preview, target);
  snapTo(zone, target);
  return true;
}

void COmarchyFloatingHyprBar::restoreSnappedSize(const Vector2D &cursor, double anchor, double offset) {
  const auto window = m_pWindow.lock();
  const auto monitor = window ? window->m_monitor.lock() : nullptr;
  if (!window || !monitor || snappedZone() == SNAP_NONE)
    return;

  const auto size = ownBox(monitor).size();
  m_preSnapBox.reset();

  auto box = window->layoutTarget()->position();

  // Back to its own size under the pointer, holding the titlebar where it was
  // grabbed, as a maximized window does.
  const auto reserved = window->getFullWindowReservedArea();
  box.w = size.x;
  box.h = size.y;
  box.x = cursor.x - box.w * anchor;
  box.y = cursor.y + reserved.topLeft.y - offset;
  g_layoutManager->setTargetGeom(box, window->layoutTarget());
  window->layoutTarget()->warpPositionSize();
}

void COmarchyFloatingHyprBar::drawSnapPreview(Render::CRenderContext &ctx, PHLMONITOR pMonitor, float a) {
  if (m_snapZone == SNAP_NONE || !m_bDraggingThis || pMonitor != m_snapMonitor.lock())
    return;

  const auto window = m_pWindow.lock();
  const auto box = CBox{m_snapPreview}.translate(-pMonitor->m_position).scale(pMonitor->m_scale).round();
  const int ROUNDING = window->presentation().rounding() * pMonitor->m_scale;

  // A faint wash in the window's border colour where it will land.
  static auto PACTIVEBORDER = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");
  const auto *GRAD = sc<Config::CGradientValueData *>(PACTIVEBORDER.ptr());
  CHyprColor wash = GRAD->m_colors.empty() ? CHyprColor{1, 1, 1, 1} : GRAD->m_colors.front();
  wash.a = 0.2F * a;

  CRectPassElement::SRectData rect;
  rect.box = box;
  rect.color = wash;
  rect.round = ROUNDING;
  rect.roundingPower = window->presentation().roundingPower();
  Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRectPassElement>(rect));
}

void COmarchyFloatingHyprBar::renderBarTitle(const Vector2D &bufferSize, const float scale) {
  const auto COLORVAL = g_pOmarchyFloatingGlobalState->config.textColor->value();
  const auto SIZE = g_pOmarchyFloatingGlobalState->config.barTextSize->value();
  const auto WEIGHT = g_pOmarchyFloatingGlobalState->config.barTextWeight->value();
  const auto FONT = g_pOmarchyFloatingGlobalState->config.barTextFont->value();
  const auto ALIGN = g_pOmarchyFloatingGlobalState->config.barTextAlign->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();

  float buttonSizes = BARBUTTONPADDING;
  for (auto &b : g_pOmarchyFloatingGlobalState->buttons) {
    buttonSizes += b.size + BARBUTTONPADDING;
  }

  const int scaledSize = std::round(SIZE * scale);
  const auto scaledButtonsSize = buttonSizes * scale;
  const auto scaledBarPadding = BARPADDING * scale;
  const int paddingTotal = scaledBarPadding * 2 + scaledButtonsSize + (ALIGN != "left" ? scaledButtonsSize : 0);
  const int maxWidth = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

  if (m_szLastTitle.empty() || maxWidth < 1) {
    m_pTextTex = nullptr;
    return;
  }

  const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
  m_pTextTex = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);
}

size_t COmarchyFloatingHyprBar::getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D &bufferSize,
                                                      const float scale) {
  float availableSpace = bufferSize.x - barPadding * scale * 2;
  size_t count = 0;

  for (const auto &button : g_pOmarchyFloatingGlobalState->buttons) {
    const float buttonSpace = (button.size + barButtonPadding) * scale;
    if (availableSpace >= buttonSpace) {
      count++;
      availableSpace -= buttonSpace;
    } else
      break;
  }

  return count;
}

void COmarchyFloatingHyprBar::renderBarButtons(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a) {
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto INACTIVECOLOR = g_pOmarchyFloatingGlobalState->config.inactiveButtonColor->value();
  const auto BUTTONROUNDING = g_pOmarchyFloatingGlobalState->config.buttonRounding->value();
  const auto BUTTONBORDERSIZE = g_pOmarchyFloatingGlobalState->config.buttonBorderSize->value();
  const auto BUTTONBORDERCOLOR = g_pOmarchyFloatingGlobalState->config.buttonBorderColor->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
  const bool INVALIDATEICONS = m_bButtonsDirty || m_bWindowSizeChanged;

  int offset = BARPADDING * scale;
  for (size_t i = 0; i < visibleCount; ++i) {
    auto &button = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto scaledButtonSize = button.size * scale;
    const auto scaledButtonsPad = BARBUTTONPADDING * scale;

    auto color = button.bgcol;

    if (INACTIVECOLOR > 0) {
      color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
      if (INVALIDATEICONS && button.userfg)
        button.iconTex.clear();
    }

    color.a *= a;

    CBox buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize : offset), barBox->y + (barBox->h - scaledButtonSize) / 2.0,
                      scaledButtonSize, scaledButtonSize};
    buttonBox.round();

    const int maxRounding = static_cast<int>(std::round(scaledButtonSize / 2.0));
    const int rounding = BUTTONROUNDING < 0 ? maxRounding : std::clamp(static_cast<int>(std::round(BUTTONROUNDING * scale)), 0, maxRounding);
    const int borderSize = std::clamp(static_cast<int>(std::round(BUTTONBORDERSIZE * scale)), 0, maxRounding);

    if (borderSize > 0) {
      auto borderColor = configColor(BUTTONBORDERCOLOR);
      borderColor.a *= a;
      g_pHyprOpenGL->renderRect(ctx, buttonBox, borderColor, {.round = rounding});
      auto face = buttonBox.copy().expand(-borderSize);
      g_pHyprOpenGL->renderRect(ctx, face, color, {.round = std::max(0, rounding - borderSize)});
    } else
      g_pHyprOpenGL->renderRect(ctx, buttonBox, color, {.round = rounding});

    // A hovered button lifts its face in its own icon color, which follows any theme.
    if (m_iButtonHoverState & (1u << i)) {
      auto hover = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);
      hover.a = 0.18 * a;
      g_pHyprOpenGL->renderRect(ctx, buttonBox, hover, {.round = rounding});
    }

    offset += scaledButtonsPad + scaledButtonSize;
  }
}

void COmarchyFloatingHyprBar::renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a) {
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ICONONHOVER = g_pOmarchyFloatingGlobalState->config.iconOnHover->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
  const auto COORDS = cursorRelativeToBar();

  int offset = BARPADDING * scale;
  float noScaleOffset = BARPADDING;

  for (size_t i = 0; i < visibleCount; ++i) {
    auto &button = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto scaledButtonSize = button.size * scale;
    const auto scaledButtonsPad = BARBUTTONPADDING * scale;

    // check if hovering here
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
    Vector2D currentPos =
        Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
    bool hovering = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + button.size + BARBUTTONPADDING, currentPos.y + button.size);
    noScaleOffset += BARBUTTONPADDING + button.size;

    auto &iconTex = button.iconTex[static_cast<int>(std::round(scale * 100))];
    if ((!iconTex || iconTex->m_texID == 0) && !button.icon.empty()) {
      // render icon
      auto fgcol = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

      iconTex = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(button.size * 0.62 * scale), false, "sans", scaledButtonSize);
    }

    if (!iconTex || iconTex->m_texID == 0)
      continue;

    const auto iconX =
        barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0) - iconTex->m_size.x / 2.0;
    const auto iconY = barBox->y + barBox->height / 2.0 - iconTex->m_size.y / 2.0;
    CBox pos = {iconX, iconY, iconTex->m_size.x, iconTex->m_size.y};
    pos.round(); // snap to whole pixels, otherwise the icon is sampled at a half-pixel offset and blurs (hyprwm/hyprland-plugins#711)

    if (!ICONONHOVER || (ICONONHOVER && m_iButtonHoverState > 0))
      g_pHyprOpenGL->renderTexture(ctx, iconTex, pos, {.a = a});
    offset += scaledButtonsPad + scaledButtonSize;

    bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
    if (hovering != currentBit) {
      m_iButtonHoverState ^= (1 << i);
      // damage to get rid of some artifacts when icons are "hidden"
      damageEntire();
    }
  }
}

void COmarchyFloatingHyprBar::draw(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a, const Render::SWindowRenderPresentation &presentation) {
  const auto ENABLED = g_pOmarchyFloatingGlobalState->config.enabled->value();

  if (m_bLastEnabledState != ENABLED) {
    m_bLastEnabledState = ENABLED;
    g_pDecorationPositioner->repositionDeco(this);
  }

  if (m_hidden || !validMapped(m_pWindow) || !ENABLED)
    return;

  const auto PWINDOW = m_pWindow.lock();

  if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
    return;

  // Under the window, over everything below it: the dragged window was raised.
  drawSnapPreview(ctx, pMonitor, a);

  auto data = COmarchyFloatingBarPassElement::SBarData{this, a};
  Render::IHyprRenderer::addPassElement(ctx, makeUnique<COmarchyFloatingBarPassElement>(data));
}

void COmarchyFloatingHyprBar::renderPass(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a) {
  const auto PWINDOW = m_pWindow.lock();

  static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
  const auto BARCOLOR = g_pOmarchyFloatingGlobalState->config.barColor->value();
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto PRECEDENCE = g_pOmarchyFloatingGlobalState->config.barPrecedenceOverBorder->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ENABLETITLE = g_pOmarchyFloatingGlobalState->config.barTitleEnabled->value();
  const auto ENABLEBLUR = g_pOmarchyFloatingGlobalState->config.barBlur->value();
  const auto INACTIVECOLOR = g_pOmarchyFloatingGlobalState->config.inactiveButtonColor->value();

  if (INACTIVECOLOR > 0) {
    bool currentWindowFocus = PWINDOW == Desktop::focusState()->window();
    if (currentWindowFocus != m_bWindowHasFocus) {
      m_bWindowHasFocus = currentWindowFocus;
      m_bButtonsDirty = true;
    }
  }

  const auto INACTIVEBAR = g_pOmarchyFloatingGlobalState->config.inactiveBarColor->value();
  const auto baseColor = PWINDOW != Desktop::focusState()->window() && INACTIVEBAR != 0 ? INACTIVEBAR : BARCOLOR;
  const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(baseColor));
  if (DEST_COLOR != m_cRealBarColor->goal())
    *m_cRealBarColor = DEST_COLOR;

  CHyprColor color = m_cRealBarColor->value();

  color.a *= a;
  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const bool SHOULDBLUR = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F;

  if (HEIGHT < 1) {
    m_iLastHeight = HEIGHT;
    return;
  }

  const auto PWORKSPACE = PWINDOW->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(PWINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  const auto ROUNDING = PWINDOW->presentation().rounding() + (PRECEDENCE ? 0 : PWINDOW->presentation().borderSize());

  const auto scaledRounding = ROUNDING > 0 ? ROUNDING * pMonitor->m_scale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

  m_seExtents = {{0, HEIGHT}, {}};

  const auto DECOBOX = assignedBoxGlobal();

  const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

  CBox titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                      DECOBOX.h + ROUNDING * 3 /* to fill the bottom cuz we can't disable rounding there */};

  titleBarBox.translate(PWINDOW->presentation().floatingOffset()).scale(pMonitor->m_scale).round();

  if (titleBarBox.w < 1 || titleBarBox.h < 1)
    return;

  g_pHyprOpenGL->scissor(ctx, titleBarBox);

  if (ROUNDING) {
    // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
    CBox windowBox = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->presentation().floatingOffset().x - pMonitor->m_position.x + 1,
                      PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->presentation().floatingOffset().y - pMonitor->m_position.y + 1,
                      PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - 2, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - 2};

    if (windowBox.w < 1 || windowBox.h < 1)
      return;

    // Hyprland's stencil pass for blurred surfaces that discard by alpha leaves
    // the write mask at 0, and a masked clear clears nothing: the bar would be
    // cut by whatever stencil that pass left behind.
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);

    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

    glStencilFunc(GL_ALWAYS, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
    g_pHyprOpenGL->renderRect(ctx, windowBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->presentation().roundingPower()});
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glStencilFunc(GL_NOTEQUAL, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  }

  g_pHyprOpenGL->renderRect(ctx, titleBarBox, color, {.round = static_cast<int>(scaledRounding), .roundingPower = PWINDOW->presentation().roundingPower(), .blur = SHOULDBLUR, .blurA = a});

  // render title
  // The app's name is steadier than a window title, which for a web app is often
  // just its domain or the page it is on. Windows no launcher claims keep theirs.
  std::string title = omarchyAppName(PWINDOW->metadata().appID());
  if (title.empty())
    title = PWINDOW->metadata().title();
  // The title is rendered at the monitor's scale. A window dragged between
  // monitors of different scales is drawn on each, so one texture is kept per
  // scale; otherwise the title shows at the other monitor's size.
  if (m_szLastTitle != title || m_bWindowSizeChanged || m_bTitleColorChanged)
    m_titleTexByScale.clear();
  if (ENABLETITLE) {
    m_szLastTitle = title;
    auto &tex = m_titleTexByScale[static_cast<int>(std::round(pMonitor->m_scale * 100))];
    if (!tex || tex->m_texID == 0) {
      renderBarTitle(BARBUF, pMonitor->m_scale);
      tex = m_pTextTex;
    }
    m_pTextTex = tex;
  }

  if (ROUNDING) {
    // cleanup stencil
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
    glStencilMask(-1);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
  }

  CBox textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};
  if (ENABLETITLE && m_pTextTex) {
    const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
    const auto ALIGN = g_pOmarchyFloatingGlobalState->config.barTextAlign->value();

    float buttonSizes = BARBUTTONPADDING;
    for (auto &b : g_pOmarchyFloatingGlobalState->buttons) {
      buttonSizes += b.size + BARBUTTONPADDING;
    }

    const auto scaledBorderSize = PWINDOW->presentation().borderSize() * pMonitor->m_scale;
    const auto scaledButtonsSize = buttonSizes * pMonitor->m_scale;
    const auto scaledBarPadding = BARPADDING * pMonitor->m_scale;
    const auto xOffset = ALIGN == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize))
                                         : std::round(((BARBUF.x - scaledBorderSize) / 2.0 - m_pTextTex->m_size.x / 2.0));
    const auto yOffset = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
    CBox titleBox = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

    g_pHyprOpenGL->renderTexture(ctx, m_pTextTex, titleBox, {.a = a});
  }

  renderBarButtons(ctx, &textBox, pMonitor->m_scale, a);
  m_bButtonsDirty = false;

  g_pHyprOpenGL->scissor(ctx, nullptr);

  renderBarButtonsText(ctx, &textBox, pMonitor->m_scale, a);

  m_bWindowSizeChanged = false;
  m_bTitleColorChanged = false;

  // dynamic updates change the extents
  if (m_iLastHeight != HEIGHT) {
    PWINDOW->layoutTarget()->recalc();
    m_iLastHeight = HEIGHT;
  }
}

eDecorationType COmarchyFloatingHyprBar::getDecorationType() { return DECORATION_CUSTOM; }

void COmarchyFloatingHyprBar::updateWindow(PHLWINDOW pWindow) {
  updateRules();
  damageEntire();
}

void COmarchyFloatingHyprBar::onConfigReloaded() {
  m_bButtonsDirty = true;
  m_bTitleColorChanged = true;
  m_pTextTex = nullptr;
  m_titleTexByScale.clear();

  g_pDecorationPositioner->repositionDeco(this);

  // Bars start disabled, so the reload that enables them has to make room for
  // them, as a change in m_hidden does.
  const auto ENABLED = g_pOmarchyFloatingGlobalState->config.enabled->value();
  if (m_bLastEnabledState != ENABLED) {
    m_bLastEnabledState = ENABLED;
    if (const auto PWINDOW = m_pWindow.lock(); PWINDOW)
      if (const auto monitor = PWINDOW->m_monitor.lock())
        monitor->m_scheduledRecalc = true;
  }

  damageEntire();
}

void COmarchyFloatingHyprBar::damageEntire() { g_pHyprRenderer->damageBox(assignedBoxGlobal()); }

Vector2D COmarchyFloatingHyprBar::cursorRelativeToBar() { return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos(); }

eDecorationLayer COmarchyFloatingHyprBar::getDecorationLayer() { return DECORATION_LAYER_UNDER; }

uint64_t COmarchyFloatingHyprBar::getDecorationFlags() {
  return DECORATION_ALLOWS_MOUSE_INPUT | (g_pOmarchyFloatingGlobalState->config.barPartOfWindow->value() ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox COmarchyFloatingHyprBar::assignedBoxGlobal() {
  if (!validMapped(m_pWindow))
    return {};

  CBox box = m_bAssignedBox;
  box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

  const auto PWORKSPACE = m_pWindow->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(m_pWindow->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW COmarchyFloatingHyprBar::getOwner() { return m_pWindow.lock(); }

void COmarchyFloatingHyprBar::updateRules() {
  const auto PWINDOW = m_pWindow.lock();
  if (!PWINDOW)
    return;
  auto prevHidden = m_hidden;
  auto prevForcedTitleColor = m_bForcedTitleColor;

  m_bForcedBarColor = std::nullopt;
  m_bForcedTitleColor = std::nullopt;
  m_hidden = false;

  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->nobarRuleIdx))
    m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->nobarRuleIdx)->effect);
  const auto workspaceTag = g_pOmarchyFloatingGlobalState->config.workspaceTag->value();
  if (!workspaceTag.empty())
    m_hidden = m_hidden || !PWINDOW->isFloating() || !PWINDOW->m_ruleApplicator->m_tagKeeper.isTagged(workspaceTag);
  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->barColorRuleIdx))
    m_bForcedBarColor = CHyprColor(
        Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->barColorRuleIdx)->effect).value_or(0));
  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->titleColorRuleIdx))
    m_bForcedTitleColor =
        CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->titleColorRuleIdx)->effect)
                       .value_or(0));

  if (prevHidden != m_hidden) {
    g_pDecorationPositioner->repositionDeco(this);
    if (const auto monitor = PWINDOW->m_monitor.lock())
      monitor->m_scheduledRecalc = true;
  }
  if (prevForcedTitleColor != m_bForcedTitleColor)
    m_bTitleColorChanged = true;
}

void COmarchyFloatingHyprBar::damageOnButtonHover() {
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

  float offset = BARPADDING;

  const auto COORDS = cursorRelativeToBar();

  unsigned int hoverState = 0;
  for (size_t i = 0; i < g_pOmarchyFloatingGlobalState->buttons.size(); ++i) {
    const auto &b = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
    Vector2D currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

    if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + BARBUTTONPADDING, currentPos.y + b.size))
      hoverState |= 1u << i;

    offset += BARBUTTONPADDING + b.size;
  }

  if (hoverState != m_iButtonHoverState) {
    m_iButtonHoverState = hoverState;
    damageEntire();
  }
}
