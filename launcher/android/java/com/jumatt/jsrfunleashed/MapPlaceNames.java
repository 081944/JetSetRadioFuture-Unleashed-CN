package com.jumatt.jsrfunleashed;

/** Transcribed from the CN SprNorm.dat models 190..212. Empty entries are
 * unused original slots, not unnamed locations to guess. */
final class MapPlaceNames {
    private static final String[] NAMES = {
        "涩谷总站", "中央街", "道玄坂", "日影街", "六角台",
        "东京地下污水设施", "希望丘", "东京地下污水设施最深处", "要塞居住区",
        "99号街", "空中恐龙广场", "六角世博体育场预定地", "零号国道",
        "摩天楼群与法老公园", null, "东京线遗址", null, null, null, null,
        null, null, "车库公园"
    };
    static String forKind(int kind) {
        int index=kind-12;
        return index>=0 && index<NAMES.length ? NAMES[index] : null;
    }
    static String forStage(int stage) {
        int group=stage>>>16,area=stage&65535,index;
        switch (group) {
        case 1: index=area; break;
        case 2: index=area+4; break;
        case 3: index=area+9; break;
        case 4: index=area<3 ? area+14 : 16; break;
        case 5: index=area+17; break;
        default: index=area+22; break;
        }
        String name=forKind(index+12);
        return name!=null ? name : "";
    }
    private MapPlaceNames() { }
}
