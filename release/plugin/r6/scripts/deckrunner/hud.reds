// Deckrunner's HUD: Skate 3's scoring cluster in Cyberpunk's own HUD style, bottom left where Skate 3 has it. The trick's
// name (with CLEAN or SKETCHY) above a segmented ring that is the line's timer, with the multiplier inside it and the
// stance up its side, and the trick's score beside it over the line's score: the layout of Skate 3's own HUD, in the
// game's blue and red. While skating, the game's action buttons there (HotkeysWidgetController: phone, car, radio,
// consumables, gadgets) fade out, as Limited HUD fades them, and come back after. And V's stance figure (bottom right)
// is a skateboard while she rides: a solid deck in its colour, in her place; on foot it's the game's own figure.
// Above the trick, Skate's session marker in Skate 3's words: Session Marker Placed; Return to Marker Position over the
// return's hold (LB + d-pad up) as a filling bar; and the same with HOLD beside it when the hold was let go too soon.
//
// Needs Codeware (which needs redscript). Without Codeware nothing here is compiled, and the HUD stays as it is. The
// values come from the Deckrunner plugin (DeckrunnerHud, declared in deckrunner.reds); the words are Skate 3's own, from
// the player's Skate 3 data. deckrunner.ini [hud] enabled=0 turns it off. It reports its steps in Deckrunner's log
// ("hud script: ...").

@if(ModuleExists("Codeware"))
public class DeckrunnerHudService extends ScriptableService {
  private let m_running: Bool;
  private let m_root: ref<inkCanvas>;
  private let m_trickRow: ref<inkHorizontalPanel>;
  private let m_trick: ref<inkText>;
  private let m_landing: ref<inkText>;
  private let m_cluster: ref<inkCanvas>;   // the ring, the stance, the multiplier and the scores
  private let m_ticks: array<ref<inkRectangle>>;
  private let m_tickAnims: array<ref<inkAnimProxy>>;
  private let m_multiplier: ref<inkText>;
  private let m_stance: ref<inkText>;
  private let m_score: ref<inkText>;
  private let m_line: ref<inkText>;
  private let m_shownTricks: Int32;
  private let m_wasVisible: Bool;
  private let m_trickAnim: ref<inkAnimProxy>;
  private let m_refreshes: Int32;
  // Skate's session marker: its message above the trick, a tag beside it (HOLD), the return's hold as a bar, and the
  // counts last seen (placed, returns, short holds).
  private let m_markerRow: ref<inkCanvas>;
  private let m_markerText: ref<inkText>;
  private let m_markerTag: ref<inkText>;
  private let m_markerBar: ref<inkCanvas>;
  private let m_markerFill: ref<inkRectangle>;
  private let m_markerAnim: ref<inkAnimProxy>;
  private let m_markerCounts: array<Int32>;
  private let m_markerHolding: Bool;
  // The game's bottom-left action buttons, faded out while skating: the buttons' controller, the hotkeys that are HUD
  // entries of their own (in 2.x the phone is: PhoneHotkeyController), and each widget faded with the opacity it had,
  // put back after.
  private let m_hotkeys: wref<HotkeysWidgetController>;
  private let m_hotkeyControllers: array<wref<inkGameController>>;
  private let m_hotkeysHidden: Bool;
  private let m_hiddenWidgets: array<wref<inkWidget>>;
  private let m_hiddenOpacity: array<Float>;
  // V's stance figure (CrouchIndicatorGameController) and the board shown in her place while she rides; her image's
  // opacity before, put back after.
  private let m_figure: wref<CrouchIndicatorGameController>;
  private let m_board: ref<inkCanvas>;
  private let m_riding: Bool;
  private let m_figureOpacity: Float;

  public static func Get() -> ref<DeckrunnerHudService> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"DeckrunnerHudService") as DeckrunnerHudService;
  }

  private static func Blue() -> HDRColor = new HDRColor(0.368627, 0.964706, 1.0, 1.0)  // ThemeColors.ElectricBlue
  private static func Red() -> HDRColor = new HDRColor(1.1761, 0.3809, 0.3476, 1.0)  // ThemeColors.Bittersweet

  // The ring: Ticks segments around a circle of RingRadius, in a RingSize square.
  private static func Ticks() -> Int32 = 48
  private static func RingSize() -> Float = 210.0
  private static func RingRadius() -> Float = 92.0
  // The marker's hold bar.
  private static func BarLength() -> Float = 420.0

  private cb func OnLoad() {
    DeckrunnerHud.Log("hud script: loaded (Codeware is here); waiting for a session");
    DeckrunnerHud.Attach(this);
    GameInstance.GetCallbackSystem().RegisterCallback(n"Session/Ready", this, n"OnSessionReady");
    GameInstance.GetCallbackSystem().RegisterCallback(n"Session/End", this, n"OnSessionEnd");
  }

  private cb func OnSessionReady(event: ref<GameSessionEvent>) {
    this.m_running = true;
    DeckrunnerHud.Log("hud script: session ready");
  }

  private cb func OnSessionEnd(event: ref<GameSessionEvent>) {
    this.m_running = false;
    this.m_root = null;  // the HUD layer goes with the session; the next one gets a new HUD
    this.m_hotkeys = null;
    ArrayClear(this.m_hotkeyControllers);
    this.m_hotkeysHidden = false;
    ArrayClear(this.m_hiddenWidgets);
    ArrayClear(this.m_hiddenOpacity);
    this.m_figure = null;
    this.m_board = null;
    this.m_riding = false;
  }

  // Our HUD on the game's HUD layer: built when it isn't there (found by name, as other HUD mods do).
  private func Ensure() {
    let window = GameInstance.GetInkSystem().GetLayer(n"inkHUDLayer").GetVirtualWindow();
    if !IsDefined(window) {
      return;
    }
    if IsDefined(this.m_root) && IsDefined(window.GetWidgetByPathName(n"DeckrunnerHud")) {
      return;
    }
    window.RemoveChildByName(n"DeckrunnerHud");
    this.Build(window);
    // The action buttons' controllers, from the HUD layer's own list (Codeware).
    this.m_hotkeys = null;
    ArrayClear(this.m_hotkeyControllers);
    for controller in GameInstance.GetInkSystem().GetLayer(n"inkHUDLayer").GetGameControllers() {
      if IsDefined(controller) && controller.IsA(n"HotkeysWidgetController") {
        this.m_hotkeys = controller as HotkeysWidgetController;
      }
      if IsDefined(controller) && controller.IsA(n"GenericHotkeyController") {
        let hotkey: wref<inkGameController> = controller as inkGameController;
        ArrayPush(this.m_hotkeyControllers, hotkey);
      }
      if IsDefined(controller) && controller.IsA(n"CrouchIndicatorGameController") {
        this.m_figure = controller as CrouchIndicatorGameController;
        this.m_board = null;
      }
    }
    DeckrunnerHud.Log("hud script: HUD " + (IsDefined(this.m_root) ? "attached to the HUD layer" : "NOT attached")
      + "; action buttons " + (IsDefined(this.m_hotkeys) ? "found" : "not found")
      + " (and " + IntToString(ArraySize(this.m_hotkeyControllers)) + " hotkeys of their own)"
      + "; stance figure " + (IsDefined(this.m_figure) ? "found" : "not found"));
  }

  // V's stance figure: riding, the board in her place (her image faded out); on foot and otherwise, the game's own.
  private func RefreshFigure(skating: Bool) {
    if !IsDefined(this.m_figure) {
      return;
    }
    let icon = this.m_figure.DeckrunnerIcon();
    if !IsDefined(icon) {
      return;
    }
    if !IsDefined(this.m_board) {
      this.BuildBoard(icon);
    }
    if !IsDefined(this.m_board) {
      return;
    }
    let riding = skating && !DeckrunnerHud.OnFoot();
    if riding {
      if !this.m_riding {
        this.m_figureOpacity = icon.GetOpacity();
      }
      icon.SetOpacity(0.0);
    } else {
      if this.m_riding {
        icon.SetOpacity(this.m_figureOpacity);
      }
    }
    this.m_board.SetVisible(riding);
    this.m_riding = riding;
  }

  // The board: a deck seen from above, in the figure's colour, where V stands in her image.
  private func BuildBoard(icon: wref<inkImage>) {
    let parent = icon.GetParentWidget() as inkCompoundWidget;
    if !IsDefined(parent) {
      return;
    }
    let size = icon.GetDesiredSize();
    if size.X < 1.0 || size.Y < 1.0 {
      size = icon.GetSize();
    }
    if size.X < 1.0 || size.Y < 1.0 {
      size = new Vector2(58.0, 134.0);
    }
    // The image sits in a horizontal panel (79 x 232 in the lab): a board canvas of its own width there became a second
    // column and V's figure went. So the board goes in right after the image with no width, and the deck is drawn back
    // over the image's place by translation.
    let index = -1;
    let i = 0;
    while i < parent.GetNumChildren() {
      if parent.GetWidgetByIndex(i) == icon {
        index = i;
      }
      i += 1;
    }
    DeckrunnerHud.Log("hud script: stance figure " + FloatToStringPrec(size.X, 0) + " x " + FloatToStringPrec(size.Y, 0)
      + ", child " + IntToString(index) + " of " + IntToString(parent.GetNumChildren()) + " in a " + NameToString(parent.GetClassName()));
    let board = new inkCanvas();
    board.SetName(n"DeckrunnerBoard");
    board.SetSize(new Vector2(0.0, size.Y));
    board.SetVisible(false);
    board.Reparent(parent, index + 1);
    // V's standing figure spans about 13% to 70% of the image's height, centred across it, and anything lower is cut off
    // (measured from the user's screenshots: a deck from 7% to 73% lost its bottom end). The deck stays inside her
    // span, about four times as long as it is wide.
    let length = size.Y * 0.52;
    let deck = DeckrunnerHudService.Deck(length / 3.8, length, icon.GetTintColor());
    deck.SetAnchor(inkEAnchor.TopLeft);
    deck.SetAnchorPoint(new Vector2(0.5, 0.5));
    deck.SetTranslation(new Vector2(-size.X * 0.5, size.Y * 0.41));
    deck.Reparent(board);
    this.m_board = board;
  }

  // A solid pill, `width` across and `length` tall with round ends: a deck seen from above. The ends are thin strips,
  // one per slice of their half circle, each a hair taller than its slice so no gap shows between them.
  private static func Deck(width: Float, length: Float, color: HDRColor) -> ref<inkCanvas> {
    let deck = new inkCanvas();
    deck.SetSize(new Vector2(width, length));
    let r = width * 0.5;
    let body = new inkRectangle();
    body.SetAnchor(inkEAnchor.TopLeft);
    body.SetSize(new Vector2(width, length - width + 1.0));
    body.SetTranslation(new Vector2(0.0, r - 0.5));
    body.SetTintColor(color);
    body.Reparent(deck);
    let n = 12;
    let h = r / Cast<Float>(n);
    let i = 0;
    while i < n {
      // Slice i lies i*h to (i+1)*h from the half circle's centre; its half-width is the circle's at the slice's middle.
      let y = (Cast<Float>(i) + 0.5) * h;
      let half = SqrtF(MaxF(r * r - y * y, 0.0));
      let strip = new inkRectangle();
      strip.SetAnchor(inkEAnchor.TopLeft);
      strip.SetSize(new Vector2(half * 2.0, h + 0.5));
      strip.SetTranslation(new Vector2(r - half, r - Cast<Float>(i + 1) * h));
      strip.SetTintColor(color);
      strip.Reparent(deck);
      let bottom = new inkRectangle();
      bottom.SetAnchor(inkEAnchor.TopLeft);
      bottom.SetSize(new Vector2(half * 2.0, h + 0.5));
      bottom.SetTranslation(new Vector2(r - half, length - r + Cast<Float>(i) * h - 0.5));
      bottom.SetTintColor(color);
      bottom.Reparent(deck);
      i += 1;
    }
    return deck;
  }

  // The bottom-left action buttons out of the way while skating, as they were otherwise.
  // Every refresh while skating, in case one of their own controllers (the phone's) faded one back in.
  private func HideHotkeys(hide: Bool) {
    if hide {
      for w in this.HotkeyWidgets() {
        if IsDefined(w) {
          if !ArrayContains(this.m_hiddenWidgets, w) {
            ArrayPush(this.m_hiddenWidgets, w);
            ArrayPush(this.m_hiddenOpacity, w.GetOpacity());
          }
          w.SetOpacity(0.0);
        }
      }
      this.m_hotkeysHidden = true;
      return;
    }
    if !this.m_hotkeysHidden {
      return;
    }
    this.m_hotkeysHidden = false;
    let i = 0;
    while i < ArraySize(this.m_hiddenWidgets) {
      let w = this.m_hiddenWidgets[i];
      if IsDefined(w) {
        w.SetOpacity(this.m_hiddenOpacity[i]);
      }
      i += 1;
    }
    ArrayClear(this.m_hiddenWidgets);
    ArrayClear(this.m_hiddenOpacity);
  }

  // The action buttons' widgets, and the roots of the hotkeys that are HUD entries of their own.
  private func HotkeyWidgets() -> array<wref<inkWidget>> {
    let widgets: array<wref<inkWidget>>;
    if IsDefined(this.m_hotkeys) {
      widgets = this.m_hotkeys.DeckrunnerWidgets();
    }
    for controller in this.m_hotkeyControllers {
      if IsDefined(controller) {
        ArrayPush(widgets, controller.GetRootWidget());
      }
    }
    return widgets;
  }

  // When what the HUD shows has changed, from the Deckrunner plugin: OnLoad hands this service over
  // (DeckrunnerHud.Attach), and the plugin calls Refresh on it. The motion in between (the trick's fade, the ring
  // draining) is ink animation. Two earlier ways failed in the lab: the game's delay system (a next-frame callback
  // re-armed from inside itself fired once per session) and the plugin looking the script up by class name.
  public func Refresh() {
    if !this.m_running {
      return;
    }
    this.Ensure();
    this.m_refreshes += 1;
    if this.m_refreshes == 1 {
      DeckrunnerHud.Log("hud script: first refresh; skating " + (DeckrunnerHud.Visible() ? "yes" : "no") + "; HUD "
        + (IsDefined(this.m_root) ? "attached" : "NOT attached"));
    }
    if !IsDefined(this.m_root) {
      return;
    }
    let visible = DeckrunnerHud.Visible();
    this.m_root.SetVisible(visible);
    this.HideHotkeys(visible);
    this.RefreshFigure(visible);
    if !visible {
      this.m_wasVisible = false;
      return;
    }
    let announced = DeckrunnerHud.NewTricks() + DeckrunnerHud.UpgradedTricks();
    let newRide = !this.m_wasVisible;
    if newRide {
      // A new ride: what was shown before belongs to the last one.
      this.m_wasVisible = true;
      this.m_shownTricks = announced;
      this.m_trickRow.SetVisible(false);
    }
    if announced != this.m_shownTricks {
      this.m_shownTricks = announced;
      this.ShowTrick();
    } else {
      // The landing comes after the trick's name.
      this.SetLanding();
    }
    this.RefreshCluster();
    this.RefreshMarker(newRide);
  }

  // Skate's session marker, in Skate 3's words: Session Marker Placed when one is placed; while LB + d-pad up is held,
  // Return to Marker Position over the hold's bar (Skate returns when it fills: 0.2 s within 100 m of the marker, up to
  // 1 s at 1 km); a hold let go too soon, the same with HOLD beside it. Each fades out after.
  private func RefreshMarker(newRide: Bool) {
    let placed = DeckrunnerHud.MarkersPlaced();
    let returns = DeckrunnerHud.MarkerReturns();
    let short = DeckrunnerHud.MarkerShortHolds();
    let progress = DeckrunnerHud.MarkerProgress();
    if newRide || ArraySize(this.m_markerCounts) != 3 {
      // A new ride: the counts start again, and nothing from the last one shows.
      this.m_markerCounts = [placed, returns, short];
      this.m_markerHolding = false;
      this.m_markerRow.SetVisible(false);
      return;
    }
    if returns > this.m_markerCounts[1] {
      this.ShowMarker(DeckrunnerHud.MarkerReturnText(), "", 1.0, 0.6);
      this.m_markerHolding = false;
    } else if short > this.m_markerCounts[2] {
      this.ShowMarker(DeckrunnerHud.MarkerReturnText(), DeckrunnerHud.HoldText(), -1.0, 2.5);
      this.m_markerHolding = false;
    } else if placed > this.m_markerCounts[0] {
      this.ShowMarker(DeckrunnerHud.MarkerPlacedText(), "", -1.0, 2.5);
      this.m_markerHolding = false;
    } else if progress > 0.0 && progress < 1.0 {
      // Held: the bar follows the hold, and stays until it returns or is let go.
      this.ShowMarker(DeckrunnerHud.MarkerReturnText(), "", progress, -1.0);
      this.m_markerHolding = true;
    } else if this.m_markerHolding && progress <= 0.0 {
      // The hold ended with neither a return nor a short hold (Skate refused it): out.
      this.m_markerHolding = false;
      this.FadeMarker(0.0, 0.3);
    }
    this.m_markerCounts = [placed, returns, short];
  }

  // The marker's message: `tag` beside it (none if empty), the hold's bar at `progress` (none below 0), and out after
  // `fadeAfter` seconds (stays below 0).
  private func ShowMarker(text: String, tag: String, progress: Float, fadeAfter: Float) {
    this.m_markerText.SetText(text);
    this.m_markerTag.SetText(tag);
    this.m_markerTag.SetVisible(StrLen(tag) > 0);
    this.m_markerBar.SetVisible(progress >= 0.0);
    this.m_markerFill.SetSize(new Vector2(DeckrunnerHudService.BarLength() * ClampF(progress, 0.0, 1.0), 6.0));
    if IsDefined(this.m_markerAnim) {
      this.m_markerAnim.Stop(true);
    }
    this.m_markerRow.SetVisible(true);
    this.m_markerRow.SetOpacity(1.0);
    if fadeAfter >= 0.0 {
      this.FadeMarker(fadeAfter, 0.5);
    }
  }

  private func FadeMarker(delay: Float, duration: Float) {
    if IsDefined(this.m_markerAnim) {
      this.m_markerAnim.Stop(true);
    }
    let anim = new inkAnimDef();
    let fade = new inkAnimTransparency();
    fade.SetStartTransparency(1.0);
    fade.SetEndTransparency(0.0);
    fade.SetStartDelay(delay);
    fade.SetDuration(duration);
    anim.AddInterpolator(fade);
    this.m_markerAnim = this.m_markerRow.PlayAnimation(anim);
  }

  private static func Text(size: Int32, style: CName, color: HDRColor) -> ref<inkText> {
    let t = new inkText();
    t.SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily");
    t.SetFontStyle(style);
    t.SetFontSize(size);
    t.SetLetterCase(textLetterCase.UpperCase);
    t.SetTintColor(color);
    t.SetFitToContent(true);
    return t;
  }

  private func Build(hudRoot: ref<inkCompoundWidget>) {
    let root = new inkCanvas();
    root.SetName(n"DeckrunnerHud");
    root.SetAnchor(inkEAnchor.Fill);
    root.SetVisible(false);
    root.Reparent(hudRoot);

    // Bottom left, where Skate 3 has it (the game's action buttons there are faded out while skating), at three
    // quarters of its first size, 5% in from the corner each way (of the HUD's 3840 x 2160 layout).
    let group = new inkCanvas();
    group.SetAnchor(inkEAnchor.BottomLeft);
    group.SetAnchorPoint(new Vector2(0.0, 1.0));
    group.SetSize(new Vector2(820.0, 300.0));
    group.SetMargin(new inkMargin(232.0, 0.0, 0.0, 198.0));
    group.SetRenderTransformPivot(new Vector2(0.0, 1.0));
    group.SetScale(new Vector2(0.75, 0.75));
    group.Reparent(root);

    let trickRow = new inkHorizontalPanel();
    trickRow.SetAnchor(inkEAnchor.TopLeft);
    trickRow.SetMargin(new inkMargin(60.0, 0.0, 0.0, 0.0));
    trickRow.SetChildMargin(new inkMargin(0.0, 0.0, 16.0, 0.0));
    trickRow.SetFitToContent(true);
    trickRow.SetVisible(false);
    trickRow.Reparent(group);
    let trick = DeckrunnerHudService.Text(52, n"Semi-Bold", DeckrunnerHudService.Blue());
    trick.SetVAlign(inkEVerticalAlign.Bottom);
    trick.Reparent(trickRow);
    let landing = DeckrunnerHudService.Text(34, n"Medium", DeckrunnerHudService.Blue());
    landing.SetVAlign(inkEVerticalAlign.Bottom);
    landing.SetMargin(new inkMargin(0.0, 0.0, 0.0, 6.0));
    landing.Reparent(trickRow);

    // Skate's session marker, above the trick: its message with a tag beside it (HOLD), and under it the return's
    // hold as a bar.
    let markerRow = new inkCanvas();
    markerRow.SetAnchor(inkEAnchor.TopLeft);
    markerRow.SetSize(new Vector2(760.0, 70.0));
    markerRow.SetMargin(new inkMargin(60.0, -84.0, 0.0, 0.0));
    markerRow.SetVisible(false);
    markerRow.Reparent(group);
    let markerLine = new inkHorizontalPanel();
    markerLine.SetAnchor(inkEAnchor.TopLeft);
    markerLine.SetChildMargin(new inkMargin(0.0, 0.0, 14.0, 0.0));
    markerLine.SetFitToContent(true);
    markerLine.Reparent(markerRow);
    let markerText = DeckrunnerHudService.Text(40, n"Semi-Bold", DeckrunnerHudService.Blue());
    markerText.SetVAlign(inkEVerticalAlign.Bottom);
    markerText.Reparent(markerLine);
    let markerTag = DeckrunnerHudService.Text(30, n"Medium", DeckrunnerHudService.Red());
    markerTag.SetVAlign(inkEVerticalAlign.Bottom);
    markerTag.SetMargin(new inkMargin(0.0, 0.0, 0.0, 4.0));
    markerTag.Reparent(markerLine);
    let markerBar = new inkCanvas();
    markerBar.SetAnchor(inkEAnchor.TopLeft);
    markerBar.SetSize(new Vector2(DeckrunnerHudService.BarLength(), 6.0));
    markerBar.SetMargin(new inkMargin(0.0, 58.0, 0.0, 0.0));
    markerBar.Reparent(markerRow);
    let track = new inkRectangle();
    track.SetAnchor(inkEAnchor.TopLeft);
    track.SetSize(new Vector2(DeckrunnerHudService.BarLength(), 6.0));
    track.SetTintColor(DeckrunnerHudService.Blue());
    track.SetOpacity(0.22);
    track.Reparent(markerBar);
    let fill = new inkRectangle();
    fill.SetAnchor(inkEAnchor.TopLeft);
    fill.SetSize(new Vector2(0.0, 6.0));
    fill.SetTintColor(DeckrunnerHudService.Blue());
    fill.Reparent(markerBar);

    let cluster = new inkCanvas();
    cluster.SetAnchor(inkEAnchor.TopLeft);
    cluster.SetSize(new Vector2(820.0, 230.0));
    cluster.SetMargin(new inkMargin(0.0, 70.0, 0.0, 0.0));
    cluster.SetVisible(false);
    cluster.Reparent(group);

    // The ring: a dim track of ticks and, over it, the lit ticks that are the line's time left.
    let ring = new inkCanvas();
    ring.SetAnchor(inkEAnchor.TopLeft);
    ring.SetSize(new Vector2(DeckrunnerHudService.RingSize(), DeckrunnerHudService.RingSize()));
    ring.SetMargin(new inkMargin(60.0, 0.0, 0.0, 0.0));
    ring.Reparent(cluster);
    let ticks: array<ref<inkRectangle>>;
    let n = DeckrunnerHudService.Ticks();
    let i = 0;
    while i < n {
      let angle = 360.0 * Cast<Float>(i) / Cast<Float>(n);
      DeckrunnerHudService.Tick(ring, angle, 0.22);
      ArrayPush(ticks, DeckrunnerHudService.Tick(ring, angle, 1.0));
      i += 1;
    }
    let multiplier = DeckrunnerHudService.Text(56, n"Semi-Bold", DeckrunnerHudService.Blue());
    multiplier.SetLetterCase(textLetterCase.LowerCase);
    multiplier.SetAnchor(inkEAnchor.Centered);
    multiplier.SetAnchorPoint(new Vector2(0.5, 0.5));
    multiplier.Reparent(ring);

    // The stance, up the ring's left side.
    let stance = DeckrunnerHudService.Text(30, n"Medium", DeckrunnerHudService.Blue());
    stance.SetAnchor(inkEAnchor.TopLeft);
    stance.SetAnchorPoint(new Vector2(0.5, 0.5));
    stance.SetMargin(new inkMargin(28.0, DeckrunnerHudService.RingSize() * 0.5, 0.0, 0.0));
    stance.SetRotation(-90.0);
    stance.Reparent(cluster);

    // The trick's score, large, beside the ring, over the line's score.
    let score = DeckrunnerHudService.Text(104, n"Semi-Bold", DeckrunnerHudService.Red());
    score.SetAnchor(inkEAnchor.TopLeft);
    score.SetMargin(new inkMargin(300.0, 34.0, 0.0, 0.0));
    score.Reparent(cluster);
    let line = DeckrunnerHudService.Text(42, n"Medium", DeckrunnerHudService.Blue());
    line.SetAnchor(inkEAnchor.TopLeft);
    line.SetMargin(new inkMargin(306.0, 142.0, 0.0, 0.0));
    line.Reparent(cluster);

    this.m_root = root;
    this.m_trickRow = trickRow;
    this.m_trick = trick;
    this.m_landing = landing;
    this.m_cluster = cluster;
    this.m_ticks = ticks;
    ArrayClear(this.m_tickAnims);
    this.m_multiplier = multiplier;
    this.m_stance = stance;
    this.m_score = score;
    this.m_line = line;
    this.m_shownTricks = -1;
    this.m_wasVisible = false;
    this.m_markerRow = markerRow;
    this.m_markerText = markerText;
    this.m_markerTag = markerTag;
    this.m_markerBar = markerBar;
    this.m_markerFill = fill;
    ArrayClear(this.m_markerCounts);
    this.m_markerHolding = false;
  }

  // One tick of the ring at `angle` degrees clockwise from the top.
  private static func Tick(ring: ref<inkCanvas>, angle: Float, opacity: Float) -> ref<inkRectangle> {
    let r = DeckrunnerHudService.RingRadius();
    let t = new inkRectangle();
    t.SetAnchor(inkEAnchor.Centered);
    t.SetAnchorPoint(new Vector2(0.5, 0.5));
    t.SetSize(new Vector2(6.0, 20.0));
    t.SetTintColor(DeckrunnerHudService.Blue());
    t.SetOpacity(opacity);
    t.SetTranslation(new Vector2(r * SinF(Deg2Rad(angle)), -r * CosF(Deg2Rad(angle))));
    t.SetRotation(angle);
    t.Reparent(ring);
    return t;
  }

  // A new trick: in with a pop, held for 3 s, then out over 1 s.
  private func ShowTrick() {
    let trick = DeckrunnerHud.Trick();
    if StrLen(trick) == 0 {
      return;
    }
    this.m_trick.SetText(trick);
    this.SetLanding();
    this.m_trickRow.SetVisible(true);
    this.m_trickRow.SetOpacity(1.0);
    if IsDefined(this.m_trickAnim) {
      this.m_trickAnim.Stop(true);
    }
    let anim = new inkAnimDef();
    let scale = new inkAnimScale();
    scale.SetStartScale(new Vector2(1.12, 1.12));
    scale.SetEndScale(new Vector2(1.0, 1.0));
    scale.SetDuration(0.15);
    anim.AddInterpolator(scale);
    let fadeIn = new inkAnimTransparency();
    fadeIn.SetStartTransparency(0.0);
    fadeIn.SetEndTransparency(1.0);
    fadeIn.SetDuration(0.12);
    anim.AddInterpolator(fadeIn);
    let fadeOut = new inkAnimTransparency();
    fadeOut.SetStartTransparency(1.0);
    fadeOut.SetEndTransparency(0.0);
    fadeOut.SetStartDelay(3.0);
    fadeOut.SetDuration(1.0);
    anim.AddInterpolator(fadeOut);
    this.m_trickRow.SetRenderTransformPivot(new Vector2(0.0, 1.0));
    this.m_trickAnim = this.m_trickRow.PlayAnimation(anim);
  }

  private func SetLanding() {
    this.m_landing.SetText(DeckrunnerHud.LandingText());
    this.m_landing.SetTintColor(DeckrunnerHud.Landing() == 2 ? DeckrunnerHudService.Red() : DeckrunnerHudService.Blue());
  }

  // The ring, the multiplier, the stance and the scores, while a line is going or a trick is scoring. The lit ticks are
  // the line's time left; each fades as the timer passes it, so the ring drains by itself until the next refresh.
  private func RefreshCluster() {
    let lineScore = DeckrunnerHud.LineScore();
    let trickScore = DeckrunnerHud.TrickScore();
    let fraction = ClampF(DeckrunnerHud.LineFraction(), 0.0, 1.0);
    let seconds = DeckrunnerHud.LineSeconds();
    let up = lineScore > 0 || trickScore > 0 || fraction > 0.0;
    this.m_cluster.SetVisible(up);
    if !up {
      return;
    }
    this.m_multiplier.SetText("x" + FloatToStringPrec(DeckrunnerHud.Multiplier(), 1));
    // The trick's stance as Skate 3 labels it (FAKIE, SWITCH), else the stance being ridden (Goofy, Regular, Switch).
    let trickStance = DeckrunnerHud.TrickStance();
    this.m_stance.SetText(StrLen(trickStance) > 0 ? trickStance : DeckrunnerHud.Stance());
    this.m_stance.SetTintColor(StrLen(trickStance) > 0 || DeckrunnerHud.Switch() ? DeckrunnerHudService.Red() : DeckrunnerHudService.Blue());
    this.m_score.SetText(IntToString(trickScore > 0 ? trickScore : lineScore));
    this.m_line.SetText(DeckrunnerHud.LineLabel() + ": " + IntToString(lineScore));

    for proxy in this.m_tickAnims {
      if IsDefined(proxy) {
        proxy.Stop(true);
      }
    }
    ArrayClear(this.m_tickAnims);
    let n = ArraySize(this.m_ticks);
    let lit = Cast<Int32>(fraction * Cast<Float>(n) + 0.5);
    let i = 0;
    while i < n {
      let tick = this.m_ticks[i];
      tick.SetOpacity(i < lit ? 1.0 : 0.0);
      if i < lit && seconds > 0.0 && fraction > 0.0 {
        // Tick i stands for the time above i/n of the timer: it goes out when the timer gets there.
        let delay = (fraction - Cast<Float>(i) / Cast<Float>(n)) / fraction * seconds;
        let anim = new inkAnimDef();
        let fade = new inkAnimTransparency();
        fade.SetStartTransparency(1.0);
        fade.SetEndTransparency(0.0);
        fade.SetStartDelay(MaxF(delay, 0.0));
        fade.SetDuration(0.08);
        anim.AddInterpolator(fade);
        ArrayPush(this.m_tickAnims, tick.PlayAnimation(anim));
      }
      i += 1;
    }
  }
}

// The action buttons' widgets: the controller's root, its slots and panel, and every hint spawned into them. The root
// and the three slots (Limited HUD's set) left the phone showing: it has its own controller (PhoneHotkeyController).
@if(ModuleExists("Codeware"))
@addMethod(HotkeysWidgetController)
public func DeckrunnerWidgets() -> array<wref<inkWidget>> {
  let widgets: array<wref<inkWidget>>;
  ArrayPush(widgets, this.GetRootWidget());
  ArrayPush(widgets, inkWidgetRef.Get(this.m_phoneSlot));
  ArrayPush(widgets, inkWidgetRef.Get(this.m_carSlot));
  ArrayPush(widgets, inkWidgetRef.Get(this.m_radioSlot));
  ArrayPush(widgets, inkWidgetRef.Get(this.m_dpadHintsPanel));
  ArrayPush(widgets, this.m_phone);
  ArrayPush(widgets, this.m_car);
  ArrayPush(widgets, this.m_radio);
  ArrayPush(widgets, this.m_consumables);
  ArrayPush(widgets, this.m_gadgets);
  ArrayPush(widgets, this.m_cyberware);
  ArrayPush(widgets, this.m_leeroy);
  ArrayPush(widgets, this.m_timeBank);
  return widgets;
}

// V's stance figure: its image (the game sets its pose; Deckrunner only fades it out while she rides).
@if(ModuleExists("Codeware"))
@addMethod(CrouchIndicatorGameController)
public func DeckrunnerIcon() -> wref<inkImage> {
  return inkWidgetRef.Get(this.m_crouchIcon) as inkImage;
}
