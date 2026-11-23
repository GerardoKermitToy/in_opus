// SPDX-License-Identifier: GPL-3.0-only
#include <winsock2.h>
#include <ws2tcpip.h>
#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "output_settings.hpp"
#include "pcm_pipeline.hpp"
#include "winamp_input.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>
using namespace winamp_opus;
namespace {
std::vector<char> Bytes(const std::filesystem::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
class Server final {
    SOCKET listener_=INVALID_SOCKET;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::vector<char> first_,second_;
    unsigned short port_=0;
    bool stall_second_=false;
    static bool Send(SOCKET s,const char* p,int bytes) {
        while(bytes>0) { const int n=send(s,p,bytes,0);if(n<=0)return false;p+=n;bytes-=n; }return true;
    }
    void Run() noexcept {
        while(!stop_) {
            fd_set r;FD_ZERO(&r);FD_SET(listener_,&r);timeval t{0,100000};
            if(select(0,&r,nullptr,nullptr,&t)<=0)continue;
            const SOCKET client=accept(listener_,nullptr,nullptr);if(client==INVALID_SOCKET)continue;
            DWORD timeout=1000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            std::string request;char buf[1024]{};
            while(request.size()<8192&&request.find("\r\n\r\n")==std::string::npos) { const int n=recv(client,buf,sizeof(buf),0);if(n<=0)break;request.append(buf,n); }
            const int index=++connections;
            constexpr char h[]="HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nicy-name: Local regression\r\nConnection: close\r\n\r\n";
            Send(client,h,sizeof(h)-1);
            if(index>1&&stall_second_) {
                stalled=true;
                while(!stop_)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else {
                const auto& data=index==1?first_:second_;
                Send(client,data.data(),static_cast<int>(data.size()));
            }
            closesocket(client);
        }
    }
public:
    std::atomic<int> connections{0};std::atomic<bool> stalled{false};
    Server(std::vector<char> a,std::vector<char>b,bool stall=false):first_(std::move(a)),second_(std::move(b)),stall_second_(stall) {
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Check(listener_!=INVALID_SOCKET,"server socket");
        sockaddr_in a4{};a4.sin_family=AF_INET;a4.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        Check(bind(listener_,reinterpret_cast<sockaddr*>(&a4),sizeof(a4))==0,"bind loopback");int n=sizeof(a4);
        Check(getsockname(listener_,reinterpret_cast<sockaddr*>(&a4),&n)==0&&listen(listener_,8)==0,"listen loopback");
        port_=ntohs(a4.sin_port);worker_=std::thread([this]{Run();});
    }
    ~Server(){stop_=true;closesocket(listener_);if(worker_.joinable())worker_.join();}
    std::wstring Url()const{return L"http://127.0.0.1:"+std::to_wstring(port_)+L"/live.opus";}
};
winamp::InModule* input=nullptr;
std::atomic<int> eof{0},titles{0},errors{0},writes{0};
std::atomic<std::int64_t> samples{0};
std::atomic<bool> writable{true};
int opens=0,closes=0,flushes=0,full_info=0,expected_channels=2;
std::wstring final_title;
void Verify(bool ok){if(!ok)++errors;}
int __cdecl Open(int rate,int channels,int bits,int,int){Verify(rate==48000&&channels==expected_channels&&bits==16);++opens;return 0;}
void __cdecl Close(){++closes;}
int __cdecl Write(char* p,int n){Verify(p&&n>0&&n<=8192&&n%(expected_channels*2)==0);samples+=n/(expected_channels*2);++writes;return 0;}
int __cdecl CanWrite(){return writable?8192:0;}
int __cdecl Playing(){return 0;}
int __cdecl Pause(int p){return p;}
void __cdecl Flush(int){++flushes;}
int __cdecl Time(){return 0;}
void __cdecl Info(int br,int rate,int channels,int sync){Verify(sync==1&&br>=0);if(rate!=-1){++full_info;Verify(rate==48&&channels==expected_channels);}}
LRESULT CALLBACK Host(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==winamp::kEndOfFile){++eof;return 0;}
    if(m==WM_USER&&l==243){++titles;wchar_t t[2048]{};input->GetFileInfo(nullptr,t,nullptr);final_title=t;return 0;}
    return DefWindowProcW(h,m,w,l);
}
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void Until(const std::function<bool()>& ready,const char* what){const auto end=GetTickCount64()+12000;while(!ready()&&GetTickCount64()<end){Pump();Sleep(1);}Check(ready(),what);}
void Reset(){Pump();eof=titles=errors=writes=0;samples=0;opens=closes=flushes=full_info=0;final_title.clear();writable=true;}
void Reconnect(const std::filesystem::path& a,const std::filesystem::path& b,int expected_titles){
    Server server(Bytes(a),Bytes(b));PlaybackSettings s;s.reconnect_attempts=1;s.reconnect_delay_ms=250;s.instant_bitrate=false;
    Check(SavePlaybackSettings(s),"save reconnect settings");input->Init();Reset();expected_channels=2;
    Check(input->Play(server.Url().c_str())==0,"play reconnect");
    Until([]{return eof.load()==1;},"reconnect EOF");input->Stop();Pump();
    Check(server.connections==2,"one bounded reconnect, no additional probe connection");
    Check(samples==2*4801,"reconnect preserves complete decoded sample counts");
    Check(opens==1&&closes==1&&flushes==0&&full_info==1,"reconnect keeps one output and format notification");
    Check(titles==expected_titles,"only changed reconnect tags notify playlist");
    Check(errors==0,"reconnect output ABI accepts unknown zero startup bitrate");
}
void StopReconnect(const std::filesystem::path& a,bool stalled){
    Server server(Bytes(a),Bytes(a),stalled);PlaybackSettings s;s.reconnect_attempts=1;s.reconnect_delay_ms=stalled?250:30000;
    Check(SavePlaybackSettings(s),"save cancelled reconnect settings");input->Init();Reset();expected_channels=2;
    Check(input->Play(server.Url().c_str())==0,"play cancel reconnect");
    Until([&]{return stalled?server.stalled.load():samples.load()==4801;},"enter reconnect wait/open");
    const auto start=GetTickCount64();input->Stop();Pump();
    Check(GetTickCount64()-start<1500,"Stop cancels reconnect delay/blocked reopen");
    Check(opens==1&&closes==1&&errors==0,"cancel reconnect cleans output once");
}
void Modes(const std::filesystem::path& source){
    for(auto mode:{OutputMode::Mono,OutputMode::Quad,OutputMode::Surround,OutputMode::SurroundII,OutputMode::LegacyUnmapped}){
        PlaybackSettings s;s.output_mode=mode;s.instant_bitrate=false;
        Check(SavePlaybackSettings(s),"save mix settings");input->Init();Reset();
        expected_channels=mode==OutputMode::Mono?1:mode==OutputMode::Quad?4:mode==OutputMode::LegacyUnmapped?6:2;
        Check(input->Play(source.c_str())==0,"play explicit output mode");Until([]{return eof.load()==1;},"mode EOF");input->Stop();Pump();
        Check(samples==5761&&opens==1&&closes==1&&titles==0&&full_info==1&&errors==0,"mode PCM/output ABI and no playlist work");
    }
}
}
int wmain(int argc,wchar_t**argv){
    RegistrySandbox registry;HMODULE dll=nullptr;HWND host=nullptr;WSADATA sockets{};
    try{
        Check(argc==3,"usage: stream_playback_tests DLL DATA_DIR");Check(WSAStartup(MAKEWORD(2,2),&sockets)==0,"WSAStartup");
        const std::filesystem::path dir(argv[2]);std::filesystem::create_directories(dir);
        const auto first=dir/L"first.opus",same=dir/L"same.opus",changed=dir/L"changed.opus",multi=dir/L"multi.opus";
        MakeOpus(first,2,4801,18001,{"ALBUM=Same"});MakeOpus(same,2,4801,18002,{"ALBUM=Same"});
        MakeOpus(changed,2,4801,18003,{"ALBUM=Changed"});MakeOpus(multi,6,5761,18004);
        dll=LoadLibraryW(argv[1]);Check(dll!=nullptr,"load new DLL");
        const auto get=reinterpret_cast<winamp::InModule*(__cdecl*)()>(GetProcAddress(dll,"winampGetInModule2"));Check(get!=nullptr,"input export");input=get();
        WNDCLASSW c{};c.lpfnWndProc=Host;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"OpusFeaturePlaybackHost";RegisterClassW(&c);
        host=CreateWindowW(c.lpszClassName,L"test",0,0,0,0,0,nullptr,nullptr,c.hInstance,nullptr);Check(host!=nullptr,"host window");
        winamp::OutModule out{};out.Open=Open;out.Close=Close;out.Write=Write;out.CanWrite=CanWrite;out.IsPlaying=Playing;out.Pause=Pause;out.Flush=Flush;out.GetOutputTime=Time;out.GetWrittenTime=Time;
        input->hMainWindow=host;input->hDllInstance=dll;input->outMod=&out;input->SetInfo=Info;
        Reconnect(first,same,0);Reconnect(first,changed,1);StopReconnect(first,false);StopReconnect(first,true);Modes(multi);
        input->Quit();FreeLibrary(dll);dll=nullptr;DestroyWindow(host);host=nullptr;WSACleanup();
        std::cout<<"Opus DLL: bounded reconnect, unchanged/ATF tags, stable output, cancellable reopen and mono/quad/matrix/raw modes passed.\n";return 0;
    }catch(const std::exception&e){if(input)input->Stop();if(dll)FreeLibrary(dll);if(host)DestroyWindow(host);WSACleanup();std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
