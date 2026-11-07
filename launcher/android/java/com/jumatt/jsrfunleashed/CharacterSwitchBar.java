package com.jumatt.jsrfunleashed;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.widget.ScrollView;
import android.widget.GridLayout;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;

/** Commands only: guest state is sampled and changed on the game thread. */
final class CharacterSwitchBar extends LinearLayout {
    private static native boolean nativeRequest(int id);
    private static native void nativeStatus(int[] out);
    private static final String[] NAMES={"Beat","Corn","Gum","Combo","Bis","Soda",
        "Yoyo","YoyoB","Galam","Sugar","Cube","LShocker","PJam","NTank",
        "Immortal","HellMets","Rapid99","Pots","Gouji","TypeA","Clutch","Jazz","TypeB","ZeroBeat"};
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final int[] state=new int[6];
    private final View[] buttons=new View[NAMES.length];
    private final TextView status;
    private final boolean floating;
    private GridLayout portraitGrid;
    private ScrollView portraitScroll;
    private int arrangedUnlocks=-1;
    private boolean attached;
    private int sequence=-1;
    private final Runnable tick=new Runnable() {
        @Override public void run() {
            if (!attached) return;
            refresh(); handler.postDelayed(this,100);
        }
    };
    CharacterSwitchBar(Context context) {
        this(context,null);
    }
    CharacterSwitchBar(Context context,Runnable afterRequest) {
        this(context,afterRequest,false);
    }
    CharacterSwitchBar(Context context,Runnable afterRequest,boolean floating) {
        super(context);
        this.floating=floating;
        setOrientation(VERTICAL);
        setGravity(Gravity.RIGHT);
        setPadding(floating ? 0 : 12,floating ? 0 : 6,floating ? 0 : 12,floating ? 0 : 6);
        if (!floating) setBackgroundColor(0xCC101820);
        status=new TextView(context);
        status.setTextColor(Color.WHITE); status.setTextSize(14);
        status.setText("进入可自由行动的场景后可切换角色");
        if (floating) {
            status.setTextSize(11); status.setGravity(Gravity.RIGHT);
            status.setPadding(dp(10),dp(6),dp(10),dp(6));
            status.setBackground(bubble(0xDC15202C,dp(16)));
        }
        LayoutParams statusParams=new LayoutParams(floating ? dp(156) : -1,-2);
        statusParams.gravity=Gravity.RIGHT; statusParams.bottomMargin=floating ? dp(8) : 0;
        if (!floating) addView(status,statusParams);
        LinearLayout row=new LinearLayout(context); row.setOrientation(floating ? VERTICAL : HORIZONTAL);
        row.setGravity(Gravity.RIGHT);
        for (int id=0;id<NAMES.length;id++) {
            final int target=id;
            View button;
            if (floating) {
                button=new CharacterBubble(context,new File(context.getExternalFilesDir(null),"game"),id);
            } else {
                Button named=new Button(context); named.setText(NAMES[id]); named.setTextSize(14);
                button=named;
            }
            buttons[id]=button;
            button.setVisibility(View.GONE);
            if (floating) {
                button.setPadding(0,0,0,0);
            }
            button.setContentDescription("切换为 "+NAMES[id]);
            button.setOnClickListener(v -> {
                if (nativeRequest(target)) {
                    status.setText("正在请求切换为 "+NAMES[target]+"…");
                    for (View b:buttons) b.setEnabled(false);
                    sequence=-1;
                    if (afterRequest!=null) afterRequest.run();
                }
            });
            LayoutParams item=new LayoutParams(floating ? dp(76) : -2,floating ? dp(76) : -2);
            item.gravity=Gravity.RIGHT; item.bottomMargin=floating ? dp(8) : 0;
            if (!floating) row.addView(button,item);
        }
        if (floating) {
            ScrollView scroll=new ScrollView(context) {
                @Override protected void onMeasure(int widthSpec,int heightSpec) {
                    // At most five WHOLE portraits in each column. Leave the
                    // objective strip clear even on a shorter secondary panel.
                    int available=getResources().getDisplayMetrics().heightPixels
                            -Math.round(getResources().getDisplayMetrics().widthPixels*82/640f)-dp(4);
                    int step=dp(80),padding=getPaddingTop()+getPaddingBottom();
                    int rows=Math.max(1,Math.min(5,(available-padding)/step));
                    int limit=rows*step+padding;
                    super.onMeasure(widthSpec,MeasureSpec.makeMeasureSpec(limit,MeasureSpec.AT_MOST));
                }
            };
            scroll.setVerticalScrollBarEnabled(false); scroll.setClipToPadding(true);
            scroll.setPadding(dp(4),0,0,0);
            scroll.setFillViewport(false);
            portraitGrid=new GridLayout(context); portraitGrid.setColumnCount(2);
            portraitGrid.setAlignmentMode(GridLayout.ALIGN_BOUNDS);
            portraitScroll=scroll;
            scroll.addView(portraitGrid); LayoutParams list=new LayoutParams(dp(164),-2);
            list.gravity=Gravity.RIGHT; addView(scroll,list);
        } else {
            HorizontalScrollView scroll=new HorizontalScrollView(context);
            scroll.setHorizontalScrollBarEnabled(false); scroll.addView(row);
            addView(scroll,new LayoutParams(-1,-2));
        }
    }
    private int dp(int value) { return Math.round(value*getResources().getDisplayMetrics().density); }
    private static GradientDrawable bubble(int color,int radius) {
        GradientDrawable shape=new GradientDrawable(); shape.setColor(color); shape.setCornerRadius(radius);
        shape.setStroke(1,0x557CDDEB); return shape;
    }
    private static String name(int id) { return id>=0 && id<NAMES.length ? NAMES[id] : "角色"; }
    private void arrangePortraits(int unlocked) {
        if (!floating || unlocked==arrangedUnlocks) return;
        arrangedUnlocks=unlocked;
        portraitGrid.removeAllViews();
        int index=0;
        for (int id=0;id<buttons.length;id++) if ((unlocked&(1<<id))!=0) {
            // Fill five down the right edge, then five down its left. Further
            // groups continue below in the same two-column scrolling grid.
            int row=(index/10)*5+index%5, column=1-(index%10)/5;
            GridLayout.LayoutParams item=new GridLayout.LayoutParams(
                    GridLayout.spec(row),GridLayout.spec(column,GridLayout.END));
            item.width=dp(76); item.height=dp(76);
            item.setMargins(dp(4),0,0,dp(4));
            portraitGrid.addView(buttons[id],item); index++;
        }
        portraitScroll.scrollTo(0,0);
    }
    private void refresh() {
        nativeStatus(state);
        if (sequence==state[5]) return;
        sequence=state[5];
        arrangePortraits(state[2]);
        for (int id=0;id<buttons.length;id++) {
            boolean unlocked=(state[2]&(1<<id))!=0;
            buttons[id].setVisibility(unlocked ? View.VISIBLE : View.GONE);
            if (!floating) ((Button)buttons[id]).setText((id==state[1] ? "✓ " : "")+NAMES[id]);
            buttons[id].setEnabled(unlocked && state[4]!=0 && id!=state[1]);
            if (floating) {
                boolean current=id==state[1];
                CharacterBubble portrait=(CharacterBubble)buttons[id];
                portrait.setCurrent(current);
                if (unlocked) portrait.loadPortrait();
                buttons[id].setAlpha(current || state[4]!=0 ? 1f : 0.55f);
                buttons[id].setContentDescription(NAMES[id]+(current ? "，当前角色" : "，点击切换"));
            }
        }
        String message;
        switch (state[0]) {
        case 2: message="正在加载 "+name(state[3])+"…"; break;
        case 3: message="正在切换角色…"; break;
        case 4: message="当前："+name(state[1]); break;
        case 5: message="请回到可自由行动的游戏画面后切换"; break;
        case 6: message="该角色尚未解锁"; break;
        case 7: message=state[1]<0 ? "角色重建未完成，请等待或重新启动游戏" : "角色切换失败，请重试"; break;
        case 8: message="切换未完成，已恢复原角色"; break;
        case 9: message="场景已改变，本次切换已取消"; break;
        default: message=state[4]!=0 ? "当前："+name(state[1])+" · 点击角色即可切换"
                : "进入可自由行动的场景后可切换角色"; break;
        }
        status.setText(message);
    }
    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow(); attached=true; sequence=-1; handler.post(tick);
    }
    @Override protected void onDetachedFromWindow() {
        attached=false; handler.removeCallbacks(tick); super.onDetachedFromWindow();
    }
}
