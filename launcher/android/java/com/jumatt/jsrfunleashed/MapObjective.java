package com.jumatt.jsrfunleashed;

/** Translate known game captions; unknown goals retain the game's live text. */
final class MapObjective {
    static String display(String caption) {
        if (caption==null) return "";
        String text=caption.replace("$n"," ").trim();
        switch (text) {
        case "Cover the graffiti at Shibuya Terminal!": return "覆盖涩谷总站的涂鸦！";
        case "Cover up all that Poison Jam graffiti!": return "覆盖所有毒果酱的涂鸦！";
        case "Cover Dogenzaka Hill in paint!": return "涂满道玄坂的涂鸦！";
        case "Cover up all of the Immortals' graffiti!": return "覆盖所有不死族的涂鸦！";
        case "Cover Rokkaku-dai Heights in graffiti!": return "涂满六角台的涂鸦！";
        case "Find the switch and spray it!": return "找到开关并喷上涂鸦！";
        case "Find the switch and stop the timer!!": return "找到开关，停止计时！！";
        case "Give 99th Street a new paint job!": return "涂满99号街的涂鸦！";
        default: return text;
        }
    }
    private MapObjective() { }
}
