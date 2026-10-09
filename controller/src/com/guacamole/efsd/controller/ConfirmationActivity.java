package com.guacamole.efsd.controller;
import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.os.UserManager;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.guacamole.efsd.IGuacamoleEfsd;
import com.guacamole.efsd.ProductionStatus;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.atomic.AtomicBoolean;
/** No command/slot/path/payload is read from Intent. A visible new confirmation is mandatory. */
public final class ConfirmationActivity extends Activity {
 private static final ExecutorService IO=Executors.newSingleThreadExecutor();
 private static final AtomicBoolean IN_FLIGHT=new AtomicBoolean();
 private TextView status;
 private boolean permitted(){UserManager u=getSystemService(UserManager.class);return UserHandle.myUserId()==0&&u!=null&&u.isAdminUser()&&!u.hasUserRestriction(UserManager.DISALLOW_CONFIG_MOBILE_NETWORKS);}
 @Override public void onCreate(Bundle b){super.onCreate(b);
  getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
  getWindow().setHideOverlayWindows(true);
  if(!permitted()){finish();return;}
  LinearLayout view=new LinearLayout(this);view.setOrientation(LinearLayout.VERTICAL);view.setPadding(32,32,32,32);
  status=new TextView(this);status.setText("Modem-wide Korea configuration only. AUTO is read-only. No manual per-SIM selection. KT/LGU network E2E and slot 2 mutation are unverified.");view.addView(status);
  button(view,"Observe status",()->observe());
  button(view,"Install Korea configuration",()->confirm(false));
  button(view,"Restore captured sel_db only",()->confirm(true));
  setContentView(view);observe();
 }
 private void button(LinearLayout view,String title,Runnable action){Button b=new Button(this);b.setText(title);b.setFilterTouchesWhenObscured(true);b.setOnClickListener(v->action.run());view.addView(b);}
 private static IGuacamoleEfsd service() throws Exception {
  var binder=ServiceManager.checkService("com.guacamole.efsd.IGuacamoleEfsd/default");
  if(binder==null)throw new Exception("Service unavailable");
  var s=IGuacamoleEfsd.Stub.asInterface(binder);if(s.getInterfaceVersion()<2)throw new Exception("Production API V2 not installed");return s;
 }
 private void show(String value){runOnUiThread(()->{if(!isFinishing()&&!isDestroyed())status.setText(value);});}
 private void observe(){if(!IN_FLIGHT.compareAndSet(false,true))return;
  IO.execute(()->{try{ProductionStatus s=service().observe();show("Generation "+s.generation+" / age "+s.ageMs+" ms\n"+s.selDb+" / "+s.recovery+"\n"+s.lastAction+": "+s.lastResult+"\nBusy: "+s.busy);}catch(Exception e){show("Observation unavailable. No retry.");}finally{IN_FLIGHT.set(false);}});
 }
 private void confirm(boolean rollback){
  if(!permitted()||IN_FLIGHT.get())return;
  AlertDialog d=new AlertDialog.Builder(this).setTitle(rollback?"Restore captured sel_db?":"Install Korea configuration?")
   .setMessage(rollback?"Explicit recovery only. This does not restore AUTO files or select Common. The original backup is preserved. No automatic retry or reboot.":"This may write modem EFS once after exact preflight. Unknown state is blocked. No automatic retry, rollback or reboot.")
   .setNegativeButton("Cancel",null).setPositiveButton("Confirm once",(dialog,which)->submit(rollback)).create();
  d.setOnShowListener(x->d.getButton(AlertDialog.BUTTON_POSITIVE).setFilterTouchesWhenObscured(true));d.show();
 }
 private void submit(boolean rollback){
  if(!permitted()||!IN_FLIGHT.compareAndSet(false,true))return;
  IO.execute(()->{try{var s=service();int result=rollback?s.rollbackKoreaConfiguration():s.installKoreaConfiguration();show("Request result: "+result+". QUEUED=0 is not completion. Observe status; do not resubmit on uncertainty.");}catch(Exception e){show("Request outcome unknown. No retry. Observe status.");}finally{IN_FLIGHT.set(false);}});
 }
}
