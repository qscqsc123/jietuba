// 可选的短生命周期 AT-SPI 查询器。隔离慢应用，宿主可随时超时终止，不阻塞框选。
// Short-lived accessibility worker: a stalled toolkit cannot block pointer input or capture.
#include <atspi/atspi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
namespace {
struct Release { template<class T> void operator()(T *p) const { if(p) g_object_unref(p); } };
using Node=std::unique_ptr<AtspiAccessible,Release>;
using Component=std::unique_ptr<AtspiComponent,Release>;
}
int main(int argc,char **argv) {
    if(argc!=6) return 1;
    const int pid=std::atoi(argv[1]), x=std::atoi(argv[2]), y=std::atoi(argv[3]);
    const int left=std::atoi(argv[4]), top=std::atoi(argv[5]);
    if(pid<=0 || atspi_init()!=0) return 1;
    atspi_set_timeout(150,300);
    Node desktop(atspi_get_desktop(0)); if(!desktop) return 1;
    for(int i=0,n=atspi_accessible_get_child_count(desktop.get(),nullptr);i<n;++i) {
        Node app(atspi_accessible_get_child_at_index(desktop.get(),i,nullptr));
        if(!app || int(atspi_accessible_get_process_id(app.get(),nullptr))!=pid) continue;
        for(int j=0,count=atspi_accessible_get_child_count(app.get(),nullptr);j<count;++j) {
            Node node(atspi_accessible_get_child_at_index(app.get(),j,nullptr));
            if (!node) continue;
            // 多窗口应用选取实际窗口。调用者还校验屏幕边界，无法定位时退回窗口。
            // Require a matching screen-space window; never guess between top-level windows of one process.
            Component comp(atspi_accessible_get_component_iface(node.get())); if(!comp) continue;
            AtspiRect *r=atspi_component_get_extents(comp.get(),ATSPI_COORD_TYPE_SCREEN,nullptr);
            bool inside=r && x>=r->x && y>=r->y && x<static_cast<long>(r->x)+r->width && y<static_cast<long>(r->y)+r->height;
            g_free(r);
            AtspiCoordType coords=ATSPI_COORD_TYPE_SCREEN;
            int px=x,py=y,dx=0,dy=0;
            if(!inside) {
                // Wayland 应用常只提供窗口内坐标；多窗口无法唯一匹配时保留窗口吸附。
                // Window-relative coordinates work without global accessibility positions. Never guess a sibling window.
                if(count!=1) continue;
                coords=ATSPI_COORD_TYPE_WINDOW; dx=left; dy=top; px-=dx; py-=dy;
            }
            bool found=false;
            for(int depth=0;depth<24;++depth) {
                Node next(atspi_component_get_accessible_at_point(comp.get(),px,py,coords,nullptr));
                if(!next || next.get()==node.get()) break;
                Component c(atspi_accessible_get_component_iface(next.get())); if(!c) break;
                node=std::move(next); comp=std::move(c); found=true;
            }
            if(!found) continue;
            r=atspi_component_get_extents(comp.get(),coords,nullptr);
            if(r) { std::printf("%d %d %d %d\n",r->x+dx,r->y+dy,r->width,r->height); g_free(r); return 0; }
        }
    }
    return 1;
}
