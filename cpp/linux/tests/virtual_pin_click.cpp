// 只供隔离的虚拟 KWin 测试使用；通过合成器分发点击，不能直接向被测控件注入事件。
// Restricted to the private test compositor: let KWin pick the target under the pointer.
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <cstdio>
#include <unistd.h>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if((argc!=3 && argc!=4) || qgetenv("WAYLAND_DISPLAY")!="wayland-jietuba-test" || qgetenv("JIETUBA_REENTRY_RESULT").isEmpty()) return 1;
    QDBusInterface remote("org.kde.KWin","/org/kde/KWin/EIS/RemoteDesktop","org.kde.KWin.EIS.RemoteDesktop");
    const QStringList gesture=argc==4 ? QString::fromLocal8Bit(argv[3]).split(':') : QStringList();
    const bool altWheel=gesture.value(0)=="alt-wheel";
    const bool drag=gesture.size()==3 && gesture.first()=="drag";
    const auto reply=remote.call("connectToEIS",altWheel ? 3 : 2); // Test-only keyboard + pointer for Alt+wheel.
    if(reply.type()==QDBusMessage::ErrorMessage || reply.arguments().size()!=2) return 2;
    auto *ctx=ei_new_sender(nullptr); ei_configure_name(ctx,"Jietuba private input test"); 
    const auto fd=reply.arguments().first().value<QDBusUnixFileDescriptor>();
    if(ei_setup_backend_fd(ctx,dup(fd.fileDescriptor()))<0) return 3;
    ei_device *motion=nullptr,*button=nullptr,*keyboard=nullptr; QElapsedTimer timeout; timeout.start();
    while(timeout.elapsed()<2500 && (!motion || !button || (altWheel && !keyboard))) {
        pollfd p{ei_get_fd(ctx),POLLIN,0}; poll(&p,1,50); ei_dispatch(ctx);
        while(auto *e=ei_get_event(ctx)) {
            if(ei_event_get_type(e)==EI_EVENT_SEAT_ADDED) {
                auto *seat=ei_event_get_seat(e);
                if(altWheel) ei_seat_bind_capabilities(seat,EI_DEVICE_CAP_POINTER_ABSOLUTE,EI_DEVICE_CAP_BUTTON,EI_DEVICE_CAP_SCROLL,EI_DEVICE_CAP_KEYBOARD,nullptr);
                else ei_seat_bind_capabilities(seat,EI_DEVICE_CAP_POINTER_ABSOLUTE,EI_DEVICE_CAP_BUTTON,EI_DEVICE_CAP_SCROLL,nullptr);
            }
            if(ei_event_get_type(e)==EI_EVENT_DEVICE_RESUMED) {
                auto *d=ei_event_get_device(e); ei_device_start_emulating(d,1);
                if(ei_device_has_capability(d,EI_DEVICE_CAP_POINTER_ABSOLUTE)) motion=ei_device_ref(d);
                if(ei_device_has_capability(d,EI_DEVICE_CAP_BUTTON)) button=ei_device_ref(d);
                if(ei_device_has_capability(d,EI_DEVICE_CAP_KEYBOARD)) keyboard=ei_device_ref(d);
            }
            ei_event_unref(e);
        }
    }
    if(!motion || !button || (altWheel && !keyboard)) return 4;
    const auto pump=[&] {
        QElapsedTimer wait; wait.start();
        while(wait.elapsed()<200) {
            pollfd fd{ei_get_fd(ctx),POLLIN,0}; poll(&fd,1,10); ei_dispatch(ctx);
            while(auto *e=ei_get_event(ctx)) {ei_event_unref(e);}
        }
    };
    ei_device_pointer_motion_absolute(motion,atoi(argv[1]),atoi(argv[2])); ei_device_frame(motion,ei_now(ctx)); pump();
    if(altWheel) {
        ei_device_keyboard_key(keyboard,KEY_LEFTALT,true); ei_device_frame(keyboard,ei_now(ctx)); pump();
        ei_device_scroll_discrete(motion,0,-120); ei_device_frame(motion,ei_now(ctx)); pump();
        ei_device_keyboard_key(keyboard,KEY_LEFTALT,false); ei_device_frame(keyboard,ei_now(ctx)); pump();
    } else for(bool pressed:{true,false}) {
        ei_device_button_button(button,BTN_LEFT,pressed); ei_device_frame(button,ei_now(ctx)); pump();
        if(pressed && drag) {
            ei_device_pointer_motion_absolute(motion,atoi(argv[1])+gesture[1].toInt(),atoi(argv[2])+gesture[2].toInt());
            ei_device_frame(motion,ei_now(ctx)); pump();
        }
    }
    if(keyboard) ei_device_unref(keyboard);
    ei_device_unref(motion);ei_device_unref(button);ei_unref(ctx); return 0;
}
