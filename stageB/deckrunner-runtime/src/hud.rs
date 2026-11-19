//! The HUD: Skate's scorer, as the plugin's Cyberpunk-style HUD shows it (ABI 13, `dkr_get_hud`). Read-only: nothing
//! here scores. The values are the ones Skate 3's own HUD movie reads (`Session::hud_input`, SK8V's overlay patch
//! 0010), and the words are Skate's own, from the string table setup converted from the player's disc
//! (`private/hud/runtime/trickdisplay.json`, its `language` map): trick names, CLEAN and SKETCHY, Switch, Fakie,
//! Regular and Goofy. Without that file the trick IDs are shown, made readable, and the plain English words.
//! ABI 14 adds Skate's session marker (`Session::marker_hud`, the engine's overlay patch 0050) and its words: Session
//! Marker Placed, Return to Marker Position, HOLD.
use std::collections::HashMap;
use std::path::Path;

use skate_host::bridge::Session;

/// What the plugin's HUD shows, after the last Skate tick. Text is UTF-8, zero-terminated.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DkrHud {
    pub size: u32,
    /// A Skate tick has filled this in since the ride started.
    pub valid: u32,
    pub tick: u64,
    /// The trick sequence being scored (a combo), and whether one is going.
    pub sequence_score: f32,
    pub sequence_active: u32,
    /// The line: its score so far, its multiplier, and the time left to keep it going (0..1 of its capacity, and
    /// seconds).
    pub line_score: f32,
    pub multiplier: f32,
    pub line_fraction: f32,
    pub line_seconds: f32,
    /// The last landing: 0 none yet, 1 clean, 2 sketchy.
    pub landing: u32,
    /// Riding switch (the opposite foot forward to the natural stance), and the natural stance is goofy.
    pub riding_switch: u32,
    pub natural_goofy: u32,
    /// Counts since the ride started: tricks announced, tricks upgraded (into a combo name), lines closed.
    pub new_tricks: u32,
    pub upgraded_tricks: u32,
    pub closed_lines: u32,
    pub trick: [u8; 96],
    pub landing_text: [u8; 24],
    pub stance_text: [u8; 24],
    pub line_label: [u8; 24],
    /// The trick's stance as Skate 3's HUD labels it beside its ring (FAKIE or SWITCH; empty for none).
    pub trick_stance: [u8; 24],
    /// ABI 14: Skate's session marker. How far LB + d-pad up's hold has run toward the return (0..1, 0 when not held;
    /// it fires at 1), whether a marker can be returned to now, and counts since the ride started: markers placed,
    /// placements refused, returns made, and return holds let go before they fired.
    pub marker_progress: f32,
    pub marker_can_return: u32,
    pub markers_placed: u32,
    pub markers_refused: u32,
    pub marker_returns: u32,
    pub marker_short_holds: u32,
    /// Skate 3's words for them.
    pub marker_placed_text: [u8; 48],
    pub marker_return_text: [u8; 48],
    pub hold_text: [u8; 16],
}

impl Default for DkrHud {
    fn default() -> Self {
        Self {
            size: std::mem::size_of::<Self>() as u32,
            valid: 0,
            tick: 0,
            sequence_score: 0.,
            sequence_active: 0,
            line_score: 0.,
            multiplier: 0.,
            line_fraction: 0.,
            line_seconds: 0.,
            landing: 0,
            riding_switch: 0,
            natural_goofy: 0,
            new_tricks: 0,
            upgraded_tricks: 0,
            closed_lines: 0,
            trick: [0; 96],
            landing_text: [0; 24],
            stance_text: [0; 24],
            line_label: [0; 24],
            trick_stance: [0; 24],
            marker_progress: 0.,
            marker_can_return: 0,
            markers_placed: 0,
            markers_refused: 0,
            marker_returns: 0,
            marker_short_holds: 0,
            marker_placed_text: [0; 48],
            marker_return_text: [0; 48],
            hold_text: [0; 16],
        }
    }
}

/// Skate's words from the player's own data: the trick names and the few HUD labels.
#[derive(Default)]
pub struct Names(HashMap<String, String>);

impl Names {
    pub const FILE: &'static str = "private/hud/runtime/trickdisplay.json";

    /// The strings the HUD needs from `<assets>/private/hud/runtime/trickdisplay.json`.
    pub fn load(assets: &Path) -> Result<Self, String> {
        let path = assets.join(Self::FILE);
        let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        Self::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))
    }

    pub fn parse(json: &[u8]) -> Result<Self, String> {
        let v: serde_json::Value = serde_json::from_slice(json).map_err(|e| e.to_string())?;
        let lang = v.get("language").and_then(|l| l.as_object()).ok_or("no language table")?;
        const KEEP: [&str; 6] = ["ID_TRICK_", "ID_SCORING_HUD_", "ID_CAC_STANCE_", "ID_TAG_", "ID_COMMON_SCORE", "ID_SESSIONMARKER_"];
        const EXACT: [&str; 1] = ["ID_REPLAY_HOLD"];
        Ok(Self(
            lang.iter()
                .filter(|(k, _)| KEEP.iter().any(|p| k.starts_with(p)) || EXACT.contains(&k.as_str()))
                .filter_map(|(k, t)| t.as_str().map(|t| (k.clone(), t.trim().to_string())))
                .collect(),
        ))
    }

    pub fn len(&self) -> usize { self.0.len() }

    fn get(&self, id: &str) -> Option<&str> { self.0.get(id).map(String::as_str).filter(|t| !t.is_empty()) }

    fn word(&self, id: &str, english: &'static str) -> String { self.get(id).unwrap_or(english).to_string() }

    /// A trick's name: Skate's text for its ID (with or without the ID_TRICK_ prefix), else the ID made readable.
    pub fn trick(&self, id: &str) -> String {
        if id.is_empty() { return String::new(); }
        let bare = id.strip_prefix("ID_TRICK_").unwrap_or(id);
        if let Some(t) = self.get(id).or_else(|| self.get(&format!("ID_TRICK_{bare}"))) { return t.to_string(); }
        bare.split('_').filter(|w| !w.is_empty()).map(|w| {
            let w = w.to_ascii_lowercase();
            if ["bs", "fs"].contains(&w.as_str()) { w.to_ascii_uppercase() } else { w[..1].to_ascii_uppercase() + &w[1..] }
        }).collect::<Vec<_>>().join(" ")
    }

    /// The stance label Skate 3's HUD puts beside its ring for a trick (its `UpdateStance`, in the trickdisplay movie):
    /// switch alone is SWITCH and fakie alone is FAKIE, nollie turns each into the other (a switch nollie is a fakie),
    /// and neither or both show nothing. The trick's name never carries the stance; nollie tricks have their own names.
    pub fn trick_stance(&self, switch: bool, fakie: bool, nollie: bool) -> String {
        let (fakie_word, switch_word) = (self.word("ID_TRICK_TYPE_FAKIE", "Fakie"), self.word("ID_TRICK_TYPE_SWITCH", "Switch"));
        match (switch, fakie) {
            (true, false) => if nollie { fakie_word } else { switch_word },
            (false, true) => if nollie { switch_word } else { fakie_word },
            _ => String::new(),
        }
    }
}

/// Counts the scorer's per-tick events over a ride and fills in `DkrHud`.
#[derive(Default)]
pub struct Tracker {
    new_tricks: u32,
    upgraded: u32,
    closed: u32,
    /// Skate's session marker counts when the ride began (placed, refused, returned, short holds): Skate keeps them
    /// for its session, which outlives a ride.
    marker_base: Option<[u32; 4]>,
}

impl Tracker {
    pub fn update(&mut self, s: &Session, tick: u64, names: &Names) -> DkrHud {
        let h = s.hud_input();
        let view = s.score();
        self.new_tricks += h.new_trick as u32;
        self.upgraded += h.modified_trick as u32;
        self.closed += h.close_tricks as u32;
        let (natural, mirrored) = s.natural_stance();
        let goofy = natural == 1;
        // Mirrored animations are the other foot forward from the natural stance: switch. (Offline: natural 1, goofy,
        // Skate's own default, and not mirrored while riding normally.)
        let riding_switch = mirrored;
        let landing = if h.clean { 1 } else if h.sketchy { 2 } else { 0 };
        let mut out = DkrHud {
            valid: 1,
            tick,
            sequence_score: finite(h.sequence_score),
            sequence_active: view.sequence_active as u32,
            line_score: finite(h.line_score),
            multiplier: finite(h.multiplier),
            line_fraction: if h.line_capacity > 0. { finite(h.line_points / h.line_capacity).clamp(0., 1.) } else { 0. },
            line_seconds: if h.line_drain > 0. { finite(h.line_points / h.line_drain).max(0.) } else { 0. },
            landing,
            riding_switch: riding_switch as u32,
            natural_goofy: goofy as u32,
            new_tricks: self.new_tricks,
            upgraded_tricks: self.upgraded,
            closed_lines: self.closed,
            ..DkrHud::default()
        };
        put(&mut out.trick, &names.trick(&h.trick_name));
        put(&mut out.trick_stance, &names.trick_stance(h.stance[0], h.stance[1], h.stance[2]));
        put(&mut out.landing_text, &match landing {
            1 => names.word("ID_SCORING_HUD_CLEAN", "CLEAN"),
            2 => names.word("ID_SCORING_HUD_SKETCHY", "SKETCHY"),
            _ => String::new(),
        });
        put(&mut out.stance_text, &if riding_switch {
            names.word("ID_TRICK_TYPE_SWITCH", "Switch")
        } else if goofy {
            names.word("ID_CAC_STANCE_GOOFY", "Goofy")
        } else {
            names.word("ID_CAC_STANCE_REGULAR", "Regular")
        });
        put(&mut out.line_label, &names.word("ID_TAG_LINE", "Line"));
        // ABI 14: Skate's session marker, counted from where this ride began.
        let m = s.marker_hud();
        let now = [m.placed, m.refused, m.returned, m.short_holds];
        let base = *self.marker_base.get_or_insert(now);
        out.marker_progress = finite(m.progress).clamp(0., 1.);
        out.marker_can_return = m.can_return as u32;
        out.markers_placed = now[0].wrapping_sub(base[0]);
        out.markers_refused = now[1].wrapping_sub(base[1]);
        out.marker_returns = now[2].wrapping_sub(base[2]);
        out.marker_short_holds = now[3].wrapping_sub(base[3]);
        put(&mut out.marker_placed_text, &names.word("ID_SESSIONMARKER_PLACED_MESSAGE", "Session Marker Placed"));
        put(&mut out.marker_return_text, &names.word("ID_SESSIONMARKER_TO_MARKER", "Return to Marker Position"));
        put(&mut out.hold_text, &names.word("ID_REPLAY_HOLD", "HOLD"));
        out
    }
}

fn finite(x: f32) -> f32 { if x.is_finite() { x } else { 0. } }

/// UTF-8 into a fixed buffer, cut on a character boundary, always zero-terminated.
fn put(dst: &mut [u8], s: &str) {
    let mut n = s.len().min(dst.len() - 1);
    while !s.is_char_boundary(n) { n -= 1; }
    dst[..n].copy_from_slice(&s.as_bytes()[..n]);
    dst[n..].fill(0);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names() -> Names {
        Names::parse(br#"{"language": {"ID_TRICK_FLIP_KICKFLIP": "Kickflip", "ID_TRICK_FLIP_DOUBLE_KICKFLIP": "Double Kickflip",
            "ID_TRICK_FLIP_NOLLIE": "Nollie", "ID_TRICK_DARKCATCH_N_360FLIP": "Nollie 360 Flip Dark Catch",
            "ID_TRICK_TYPE_SWITCH": "Switch", "ID_TRICK_TYPE_FAKIE": "Fakie", "ID_SCORING_HUD_CLEAN": "CLEAN",
            "ID_CAC_STANCE_REGULAR": "Regular", "ID_CHALLENGE_SOMETHING": "not kept",
            "ID_SESSIONMARKER_PLACED_MESSAGE": "Session Marker Placed", "ID_REPLAY_HOLD": "HOLD", "ID_REPLAY_HOLDING": "not kept",
            "ID_PHONELIST_PLACE_MARKER": "not kept"}}"#).unwrap()
    }

    #[test]
    fn names_are_skates_own() {
        let n = names();
        assert_eq!(n.len(), 10, "only the HUD's strings are kept");
        assert_eq!(n.trick("ID_TRICK_FLIP_DOUBLE_KICKFLIP"), "Double Kickflip");
        assert_eq!(n.trick("FLIP_KICKFLIP"), "Kickflip", "the ID without its prefix");
        assert_eq!(n.trick("ID_TRICK_GRIND_BS_LIP_SLIDE"), "Grind BS Lip Slide", "an unknown ID, made readable");
        assert_eq!(n.trick(""), "");
    }

    #[test]
    fn the_stance_label_follows_skates_hud() {
        let n = names();
        assert_eq!(n.trick_stance(true, false, false), "Switch");
        assert_eq!(n.trick_stance(true, false, true), "Fakie", "a switch nollie is a fakie");
        assert_eq!(n.trick_stance(false, true, false), "Fakie");
        assert_eq!(n.trick_stance(false, true, true), "Switch");
        assert_eq!(n.trick_stance(false, false, true), "", "a nollie alone says so in its name");
        assert_eq!(n.trick_stance(true, true, false), "");
        assert_eq!(n.trick("ID_TRICK_DARKCATCH_N_360FLIP"), "Nollie 360 Flip Dark Catch", "the name as Skate has it");
    }

    #[test]
    fn the_session_markers_words_are_skates_own() {
        let n = names();
        assert_eq!(n.word("ID_SESSIONMARKER_PLACED_MESSAGE", "Session Marker Placed"), "Session Marker Placed");
        assert_eq!(n.word("ID_REPLAY_HOLD", "HOLD"), "HOLD");
        assert_eq!(n.word("ID_SESSIONMARKER_TO_MARKER", "Return to Marker Position"), "Return to Marker Position",
                   "missing from the data: the English");
    }

    #[test]
    fn the_hud_is_laid_out_for_the_plugin() {
        // The plugin's mirror (stageA/probe/src/StageB.hpp) asserts the same.
        assert_eq!(std::mem::size_of::<DkrHud>(), 392);
        assert_eq!(std::mem::offset_of!(DkrHud, trick), 64);
        assert_eq!(std::mem::offset_of!(DkrHud, marker_progress), 256);
    }

    #[test]
    fn text_is_cut_on_a_character_boundary() {
        let mut b = [0xffu8; 6];
        put(&mut b, "Kickflip");
        assert_eq!(&b, b"Kickf\0");
        put(&mut b, "a\u{e9}\u{e9}\u{e9}");  // 1 + 2 + 2 + 2 bytes: the third é doesn't fit
        assert_eq!(&b[..6], b"a\xc3\xa9\xc3\xa9\0");
    }
}
