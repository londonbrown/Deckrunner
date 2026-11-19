// What the Deckrunner plugin gives the scripts: Skate's scorer for the HUD (hud.reds, with Codeware) and the weapon
// wheel's pick while skating (wheel.reds). The plugin registers this class with the game.

public native class DeckrunnerHud extends IScriptable {
  public static native func Visible() -> Bool
  public static native func Skating() -> Bool
  public static native func OnFoot() -> Bool
  public static native func Switch() -> Bool
  public static native func TrickScore() -> Int32
  public static native func LineScore() -> Int32
  public static native func Multiplier() -> Float
  public static native func LineFraction() -> Float
  public static native func LineSeconds() -> Float
  public static native func Landing() -> Int32
  public static native func NewTricks() -> Int32
  public static native func UpgradedTricks() -> Int32
  public static native func ClosedLines() -> Int32
  public static native func Trick() -> String
  public static native func LandingText() -> String
  public static native func Stance() -> String
  public static native func LineLabel() -> String
  public static native func TrickStance() -> String
  public static native func MarkerProgress() -> Float
  public static native func MarkersPlaced() -> Int32
  public static native func MarkerReturns() -> Int32
  public static native func MarkerShortHolds() -> Int32
  public static native func MarkerPlacedText() -> String
  public static native func MarkerReturnText() -> String
  public static native func HoldText() -> String
  // The weapon wheel's pick while skating (wheel.reds), and a line in Deckrunner's log.
  public static native func WheelPick(item: ItemID)
  public static native func Log(text: String)
  // The HUD script hands itself over; the plugin calls its Tick() every frame.
  public static native func Attach(service: ref<IScriptable>)
}
