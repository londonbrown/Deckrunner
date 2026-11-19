// Deckrunner's weapon wheel while skating (issue #3). On foot the plugin lets the wheel open (it lifts the
// NoRadialMenus restriction); the skating restrictions still refuse a pick, so the pick comes here, from the wheel's
// own DrawItem, and goes to the plugin instead: it leaves skating, and draws the picked weapon unless it was the
// skateboard. The wheel then shuts as after any pick. Needs only redscript (which TweakXL and ArchiveXL need too).

// Its presence tells the plugin the wheel can be offered on foot.
public class DeckrunnerWheel {}

@wrapMethod(RadialWheelController)
private func DrawItem(slot: ref<RadialSlot>) -> Bool {
  if DeckrunnerHud.Skating() {
    let item = InventoryItemData.GetID(this.GetController(slot).GetItemData());
    if !ItemID.IsValid(item) {
      return false;
    }
    DeckrunnerHud.WheelPick(item);
    return true;
  }
  return wrappedMethod(slot);
}
