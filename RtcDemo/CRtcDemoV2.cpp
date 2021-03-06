﻿// CRtcDemoV2.cpp : implementation file
//
#include "stdafx.h"
#include "CRtcDemoV2.h"
#include "afxdialogex.h"
#include "resource.h"
#include <fstream>
#include "Global.h"
#include "qn_rtc_errorcode.h"

#define CHECK_CALLBACK_TIMER 10001       // SDK 事件驱动定时器
#define UPDATE_TIME_DURATION_TIMER 10002 // 定时更新连麦时长

UINT TrackInfoUI::WINDOW_ID = 1000000;

// 服务器合流配置的画布大小，后台配置
#define CANVAS_WIDTH 480
#define CANVAS_HEIGHT 848

#define CAMERA_TAG "camera"
#define MICROPHONE_TAG "microphone"
#define SCREENCASTS_TAG "screen"
#define EXTERNAL_TAG "external"


// CRtcDemoV2 dialog

IMPLEMENT_DYNAMIC(CRtcDemoV2, CDialogEx)

#define VOLUMEMAX   32767
#define VOLUMEMIN	-32768

#ifndef core_min
#define core_min(a, b)		((a) < (b) ? (a) : (b))
#endif

//获取音频分贝值
static uint32_t ProcessAudioLevel(const int16_t* data, const int32_t& data_size)
{
    uint32_t ret = 0;

    if (data_size > 0) {
        int32_t sum = 0;
        int16_t* pos = (int16_t *)data;
        for (int i = 0; i < data_size; i++) {
            sum += abs(*pos);
            pos++;
        }

        ret = sum * 500.0 / (data_size * VOLUMEMAX);
        ret = core_min(ret, 100);
    }

    return ret;
}

CRtcDemoV2::CRtcDemoV2(CWnd* main_dlg, CWnd* pParent /*=nullptr*/)
    : CDialogEx(IDD_DIALOG_V2, pParent)
    , _main_dlg_ptr(main_dlg)
{
}

CRtcDemoV2::~CRtcDemoV2()
{
}

void CRtcDemoV2::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_PLAYER, _user_list_ctrl);
    DDX_Control(pDX, IDC_RICHEDIT_MSG, _msg_rich_edit_ctrl);
}

BOOL CRtcDemoV2::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    ReadConfigFile();

    std::string ver;
    qiniu_v2::QNRoomInterface::GetVersion(ver);
    TRACE("Sdk version: %s", ver.c_str());
    qiniu_v2::QNRoomInterface::SetLogParams(qiniu_v2::LOG_INFO, "rtc_log", "rtc.log");
    
    _rtc_room_interface = qiniu_v2::QNRoomInterface::ObtainRoomInterface();
    _rtc_room_interface->SetRoomListener(this);

    _rtc_video_interface = _rtc_room_interface->ObtainVideoInterface();
    _rtc_video_interface->SetVideoListener(this);
    _rtc_audio_interface = _rtc_room_interface->ObtainAudioInterface();
    _rtc_audio_interface->SetAudioListener(this);
        
    // 定义一个定时器，在主线程定时执行 QNRoomInterface::Loop() 方法，以触发各种回调
    // 目的是为了让 SDK 的回调在主线程中执行，以方便 UI 操作
    SetTimer(CHECK_CALLBACK_TIMER, 10, nullptr);

    InitUI();

    return TRUE;
}

BEGIN_MESSAGE_MAP(CRtcDemoV2, CDialogEx)
    ON_WM_DESTROY()
    ON_BN_CLICKED(IDCANCEL, &CRtcDemoV2::OnBnClickedCancel)
    ON_WM_CREATE()
    ON_WM_TIMER()
    ON_MESSAGE(MERGE_MESSAGE_ID, &CRtcDemoV2::OnHandleMessage)
    ON_BN_CLICKED(IDC_BUTTON_LOGIN, &CRtcDemoV2::OnBnClickedButtonLogin)
    ON_BN_CLICKED(IDC_BUTTON_PREVIEW_VIDEO, &CRtcDemoV2::OnBnClickedButtonPreviewVideo)
    ON_BN_CLICKED(IDC_BTN_FLUSH, &CRtcDemoV2::OnBnClickedBtnFlush)
    ON_BN_CLICKED(IDC_BUTTON_PREVIEW_SCREEN, &CRtcDemoV2::OnBnClickedButtonPreviewScreen)
    ON_BN_CLICKED(IDC_CHECK_CAMERA, &CRtcDemoV2::OnBnClickedCheckCamera)
    ON_BN_CLICKED(IDC_CHECK_SCREEN, &CRtcDemoV2::OnBnClickedCheckScreen)
    ON_BN_CLICKED(IDC_CHECK_AUDIO, &CRtcDemoV2::OnBnClickedCheckAudio)
    ON_BN_CLICKED(IDC_CHECK_IMPORT_RAW_DATA, &CRtcDemoV2::OnBnClickedCheckImportRawData)
    ON_BN_CLICKED(IDC_CHECK_DESKTOP_AUDIO, &CRtcDemoV2::OnBnClickedCheckDesktopAudio)
    ON_BN_CLICKED(IDC_BTN_KICKOUT, &CRtcDemoV2::OnBnClickedBtnKickout)
    ON_BN_CLICKED(IDC_CHECK_MUTE_AUDIO, &CRtcDemoV2::OnBnClickedCheckMuteAudio)
    ON_BN_CLICKED(IDC_CHECK_MUTE_VIDEO, &CRtcDemoV2::OnBnClickedCheckMuteVideo)
    ON_WM_HSCROLL()
    ON_CBN_SELCHANGE(IDC_COMBO_MICROPHONE, &CRtcDemoV2::OnCbnSelchangeComboMicrophone)
    ON_CBN_SELCHANGE(IDC_COMBO_PLAYOUT, &CRtcDemoV2::OnCbnSelchangeComboPlayout)
    ON_BN_CLICKED(IDC_BUTTON_MERGE, &CRtcDemoV2::OnBnClickedButtonMerge)
END_MESSAGE_MAP()


// CRtcDemoV2 message handlers

void CRtcDemoV2::OnDestroy()
{
    // 结束外部数据导入的线程
    _stop_external_flag = true;
    if (_fake_audio_thread.joinable()) {
        _fake_audio_thread.join();
    }
    if (_fake_video_thread.joinable()) {
        _fake_video_thread.join();
    }
    // 释放 RTC SDK 资源
    if (_rtc_room_interface) {
        StopPublish();
        _rtc_room_interface->LeaveRoom();
        qiniu_v2::QNRoomInterface::DestroyRoomInterface(_rtc_room_interface);
        _rtc_room_interface = nullptr;
    }
    CDialogEx::OnDestroy();
}

void CRtcDemoV2::OnBnClickedCancel()
{
    // TODO: Add your control notification handler code here
    CDialogEx::OnCancel();
    
    // 通知 V1 主界面显示
    //::PostMessage(_main_dlg_ptr->m_hWnd, WM_SHOWWINDOW, 0, 0);
}

void CRtcDemoV2::OnTimer(UINT_PTR nIDEvent)
{
    if (CHECK_CALLBACK_TIMER == nIDEvent) {
        // SDK 事件驱动器
        if (_rtc_room_interface) {
            _rtc_room_interface->Loop();
        }
    } else if (UPDATE_TIME_DURATION_TIMER == nIDEvent)
    {
        // 更新连麦时间
        chrono::seconds df_time
            = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - _start_tp);
        int hour = df_time.count() / 3600;
        int minute = df_time.count() % 3600 / 60;
        int sec = df_time.count() % 3600 % 60;
        wchar_t time_buf[128] = { 0 };
        wsprintf(time_buf,
            _T("连麦时长：%02d:%02d:%02d"),
            hour,
            minute,
            sec
        );
        _wnd_status_bar.SetText(time_buf, 0, 0);
    }
}

void CRtcDemoV2::ReadConfigFile()
{
    ifstream is("config");
    if (is.bad()) {
        return;
    }
    char appId_buf[128] = { 0 };
    char room_buf[128] = { 0 };
    char user_buf[128] = { 0 };
    if (!is.getline(appId_buf, 128)) {
        // 默认值
        SetDlgItemText(IDC_EDIT_APPID, utf2unicode("d8lk7l4ed").c_str());
        return;
    }
    if (!is.getline(room_buf, 128)) {
        return;
    }
    if (!is.getline(user_buf, 128)) {
        return;
    }
    SetDlgItemText(IDC_EDIT_APPID, utf2unicode(appId_buf).c_str());
    SetDlgItemText(IDC_EDIT_ROOM_ID, utf2unicode(room_buf).c_str());
    SetDlgItemText(IDC_EDIT_PLAYER_ID, utf2unicode(user_buf).c_str());
    is.close();
}

void CRtcDemoV2::WriteConfigFile()
{
    ofstream os("config");
    if (os.bad()) {
        return;
    }
    os.clear();
    string app_id = unicode2utf(_app_id.GetBuffer());
    string room_name = unicode2utf(_room_name.GetBuffer());
    string user_id = unicode2utf(_user_id.GetBuffer());
    os.write(app_id.c_str(), app_id.size());
    os.write("\n", 1);
    os.write(room_name.c_str(), room_name.size());
    os.write("\n", 1);
    os.write(user_id.c_str(), user_id.size());
    os.close();
}

void CRtcDemoV2::InitUI()
{
    _wnd_status_bar.Create(WS_CHILD | WS_VISIBLE | SBT_OWNERDRAW, CRect(0, 0, 0, 0), this, 0);
    RECT rc;
    GetWindowRect(&rc);
    int strPartDim[3] = { rc.right / 5, rc.right / 5 * 3, -1 };
    _wnd_status_bar.SetParts(3, strPartDim);
    //设置状态栏文本 
    _wnd_status_bar.SetText(_T("通话时长：00:00::00"), 0, 0);
    _wnd_status_bar.SetText(_T("连麦状态"), 1, 0);
    _wnd_status_bar.SetText(utf2unicode(GetAppVersion(__DATE__, __TIME__)).c_str(), 2, 0);

    // 初始化音量控制条配置
    ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_RECORD))->SetRange(0, 100);
    ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_RECORD))->SetPos(100);
    ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_PLAYOUT))->SetRange(0, 100);
    ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_PLAYOUT))->SetPos(100);

    // 初始化用户列表控件
    _user_list_ctrl.SetExtendedStyle(LVS_EX_FULLROWSELECT);
    _user_list_ctrl.InsertColumn(0, _T("用户 ID"), LVCFMT_LEFT, 100, 0);    //设置列
    _user_list_ctrl.InsertColumn(1, _T("用户发布流状态"), LVCFMT_LEFT, 350, 1);

    // 初始化视频采集设备 combobox
    int camera_count = _rtc_video_interface->GetCameraCount();
    for (int i(0); i < camera_count; ++i) {
        qiniu_v2::CameraDeviceInfo ci = _rtc_video_interface->GetCameraInfo(i);
        _camera_dev_map[ci.device_id] = ci;
        ((CComboBox *)GetDlgItem(IDC_COMBO_CAMERA))->InsertString(-1, utf2unicode(ci.device_name).c_str());
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_CAMERA))->SetCurSel(0);

    // 初始化屏幕窗口列表
    int screen_count = _rtc_video_interface->GetScreenWindowCount();
    for (int i(0); i < screen_count; ++i) {
        qiniu_v2::ScreenWindowInfo sw = _rtc_video_interface->GetScreenWindowInfo(i);
        _screen_info_map[sw.id] = sw;
        ((CComboBox *)GetDlgItem(IDC_COMBO_SCREEN))->InsertString(-1, utf2unicode(sw.title).c_str());
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_SCREEN))->SetCurSel(0);

    // 初始化音频采集设备列表
    int audio_rec_count = _rtc_audio_interface->GetAudioDeviceCount(qiniu_v2::AudioDeviceInfo::adt_record);
    for (int i(0); i < audio_rec_count; ++i) {
        qiniu_v2::AudioDeviceInfo audio_info;
        if (_rtc_audio_interface->GetAudioDeviceInfo(qiniu_v2::AudioDeviceInfo::adt_record, i, audio_info) == 0) {
            ((CComboBox *)GetDlgItem(IDC_COMBO_MICROPHONE))->InsertString(
                -1,
                utf2unicode(audio_info.device_name).c_str()
            );
            if (audio_info.is_default) {
                qiniu_v2::AudioDeviceSetting ads;
                ads.device_index = audio_info.device_index;
                ads.device_type = qiniu_v2::AudioDeviceSetting::wdt_DefaultDevice;
                _rtc_audio_interface->SetRecordingDevice(ads);
            }
            _microphone_dev_map[i] = audio_info;
        }
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_MICROPHONE))->SetCurSel(0);

    // 初始化音频播放设备列表
    int audio_play_count = _rtc_audio_interface->GetAudioDeviceCount(qiniu_v2::AudioDeviceInfo::adt_playout);
    for (int i(0); i < audio_play_count; ++i) {
        qiniu_v2::AudioDeviceInfo audio_info;
        if (_rtc_audio_interface->GetAudioDeviceInfo(qiniu_v2::AudioDeviceInfo::adt_playout, i, audio_info) == 0) {
            ((CComboBox *)GetDlgItem(IDC_COMBO_PLAYOUT))->InsertString(
                -1,
                utf2unicode(audio_info.device_name).c_str()
            );
            if (audio_info.is_default) {
                qiniu_v2::AudioDeviceSetting ads;
                ads.device_index = audio_info.device_index;
                ads.device_type = qiniu_v2::AudioDeviceSetting::wdt_DefaultDevice;
                _rtc_audio_interface->SetPlayoutDevice(ads);
            }
            _playout_dev_map[i] = audio_info;
        }
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_PLAYOUT))->SetCurSel(0);

    ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->SetCheck(1);
    ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->SetCheck(0);
    ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->SetCheck(1);
    ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->SetCheck(0);
}

std::tuple<int, int> CRtcDemoV2::FindBestVideoSize(const qiniu_v2::CameraCapabilityVec& camera_cap_vec_)
{
    if (camera_cap_vec_.empty()) {
        return{ 0,0 };
    }
    // 高宽比例
    float wh_ratio = 1.0f * 3 / 4;
    int dest_width(0), dest_height(0);
    for (auto itor : camera_cap_vec_) {
        if ((1.0f * itor.height / itor.width) == wh_ratio) {
            if (itor.width >= 480) {
                dest_width = itor.width;
                dest_height = itor.height;
            }
        }
    }
    if (dest_width == 0 || dest_height == 0) {
        dest_width = camera_cap_vec_.back().width;
        dest_height = camera_cap_vec_.back().height;
    }
    return std::make_tuple(dest_width, dest_height);
}

void CRtcDemoV2::OnJoinResult(
    int error_code_,
    const string& error_str_,
    const qiniu_v2::UserInfoList& user_vec_,
    const qiniu_v2::TrackInfoList& tracks_vec_,
    bool reconnect_flag_)
{
    GetDlgItem(IDC_BUTTON_LOGIN)->EnableWindow(TRUE);

    _start_tp = chrono::steady_clock::now();
    if (0 == error_code_) {
        _wnd_status_bar.SetText(_T("登录成功！"), 1, 0);
        SetDlgItemText(IDC_BUTTON_LOGIN, _T("离开"));

        lock_guard<recursive_mutex> lck(_mutex);
        _user_list.clear();
        _user_list_ctrl.DeleteAllItems();
        for each (qiniu_v2::UserInfo itor in user_vec_)
        {
            _user_list_ctrl.InsertItem(0, utf2unicode(itor.user_id).c_str());
            _user_list_ctrl.SetItemText(0, 0, utf2unicode(itor.user_id).c_str());

            _user_list.push_back(itor.user_id);
        }

        _local_tracks_list.clear();
        _remote_tracks_map.clear();
        qiniu_v2::TrackInfoList sub_tracks_list;

        for (auto&& itor : tracks_vec_) {
            if (itor->GetUserId().compare(unicode2utf(_user_id.GetBuffer())) == 0) {
                continue;
            }
            TRACE("%s", itor->GetKind());
            auto tmp_track_ptr = qiniu_v2::QNTrackInfo::Copy(itor);
            // 自动订阅
            shared_ptr<TrackInfoUI> tiu(new TrackInfoUI(this, tmp_track_ptr));
            if (tiu->render_wnd_ptr) {
                tmp_track_ptr->SetRenderHwnd((void*)tiu->render_wnd_ptr->m_hWnd);
            }
            _remote_tracks_map.insert_or_assign(tmp_track_ptr->GetTrackId(), tiu);
            sub_tracks_list.emplace_back(tmp_track_ptr);
            itor->Release();
        }
        if (!sub_tracks_list.empty()) {
            _rtc_room_interface->SubscribeTracks(sub_tracks_list);
            // 调整订阅窗口的布局
            AdjustSubscribeLayouts();
        }

        SetTimer(UPDATE_TIME_DURATION_TIMER, 100, nullptr);

        // 主动配置当前音频设备，也可以不调用，则 SDK 使用系统默认设备
        OnCbnSelchangeComboMicrophone();
        OnCbnSelchangeComboPlayout();

        if (!reconnect_flag_) {
            // 自动开始发布
            StartPublish();
        }
        _msg_rich_edit_ctrl.SetWindowTextW(_T(""));
        _msg_rich_edit_ctrl.UpdateData();
        _msg_rich_edit_ctrl.Invalidate();

        if (1 == ((CButton*)GetDlgItem(IDC_CHECK_MERGE))->GetCheck()) {
            CreateCustomMergeJob();
        }

    } else {
        _wnd_status_bar.SetText((_T("登录失败：") + utf2unicode(error_str_)).c_str(), 1, 0);
        SetDlgItemText(IDC_BUTTON_LOGIN, _T("登录"));

        // ReJoin
        PostMessage(WM_COMMAND, MAKEWPARAM(IDC_BUTTON_LOGIN, BN_CLICKED), NULL);
    }
}

void CRtcDemoV2::OnLeave(int error_code_, const string& error_str_, const string& user_id_)
{
    wchar_t buff[1024] = { 0 };
    _snwprintf(
        buff, 
        1024, 
        _T("您以非正常方式离开了房间：error code:%d, error msg:%s, kickout user:%s"), 
        error_code_, 
        utf2unicode(error_str_).c_str(),
        utf2unicode(user_id_).c_str()
    );
    _wnd_status_bar.SetText(buff, 1, 0);
    CString msg_str(buff);

    thread([&, msg_str] {
        MessageBox(msg_str);
    }).detach();
    
    KillTimer(UPDATE_TIME_DURATION_TIMER);
    Invalidate();

    // 发送消息离开房间，并释放 SDK 资源
    PostMessage(WM_COMMAND, MAKEWPARAM(IDC_BUTTON_LOGIN, BN_CLICKED), NULL);
}

void CRtcDemoV2::OnRoomStateChange(qiniu_v2::RoomState state_)
{
    if (state_ == qiniu_v2::RoomState::rs_reconnecting) {
        _wnd_status_bar.SetText(_T("网络断开，自动重连中，请检查您的网络！"), 1, 0);
    }
}

void CRtcDemoV2::OnPublishTracksResult(int error_code_, 
    const string& error_str_, const qiniu_v2::TrackInfoList& track_info_list_)
{
    lock_guard<recursive_mutex> lck(_mutex);
    wchar_t buff[1024] = { 0 };
    for (auto&& itor : track_info_list_) {
        // 还原控件状态 
        if (itor->GetTag().compare(CAMERA_TAG) == 0) {
            GetDlgItem(IDC_CHECK_CAMERA)->EnableWindow(TRUE);
        } else if (itor->GetTag().compare(MICROPHONE_TAG) == 0) {
            GetDlgItem(IDC_CHECK_AUDIO)->EnableWindow(TRUE);
        } else if (itor->GetTag().compare(SCREENCASTS_TAG) == 0) {
            GetDlgItem(IDC_CHECK_SCREEN)->EnableWindow(TRUE);
        } else if (itor->GetTag().compare(EXTERNAL_TAG) == 0) {
            GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA)->EnableWindow(TRUE);
        }

        if (itor->GetTrackId().empty()) {
            // 此 Track 发布失败
            _snwprintf(buff, 1024, _T("本地流发布失败，Tag：%d"), utf2unicode(itor->GetTag()));
            _wnd_status_bar.SetText(buff, 1, 0);
            continue;
        }
        _local_tracks_list.emplace_back(qiniu_v2::QNTrackInfo::Copy(itor));
        if (itor->GetSourceType() == qiniu_v2::tst_ExternalYUV) {
            ImportExternalRawFrame(itor->GetTrackId());
        }
    }
    if (0 == error_code_) {
        _snwprintf(buff, 1024, _T("本地流发布成功，流数量：%d"), track_info_list_.size());
    } else {
        _snwprintf(buff, 1024, _T("本地流发布失败，error code:%d, error msg:%s"), 
            error_code_, utf2unicode(error_str_).c_str());
    }
    // 释放 SDK 内部资源
    for (auto&& itor : track_info_list_)
    {
        itor->Release();
    }
    _wnd_status_bar.SetText(buff, 1, 0);
    AdjustMergeStreamLayouts();
}

void CRtcDemoV2::OnSubscribeTracksResult(int error_code_, 
    const std::string &error_str_, const qiniu_v2::TrackInfoList &track_info_list_)
{
    wchar_t buff[1024] = { 0 };
    int succ_num(0), failed_num(0);
    // 依次判断订阅结果
    for (auto&& itor : track_info_list_)
    {
        if (itor->IsConnected()) {
            ++succ_num;
            TRACE(
                _T("订阅成功， User Id:%s, track Id：%s, Kind:%s, tag:%s\n"),
                utf2unicode(itor->GetUserId()).c_str(),
                utf2unicode(itor->GetTrackId()).c_str(),
                utf2unicode(itor->GetKind()).c_str(),
                utf2unicode(itor->GetTag()).c_str()
            );
        } else {
            ++failed_num;
            // 释放已分配的资源（渲染窗口等）
            auto itor_ui = _remote_tracks_map.find(itor->GetTrackId());
            if (itor_ui != _remote_tracks_map.end()) {
                _remote_tracks_map.erase(itor_ui);
            }
            TRACE(
                _T("订阅失败， User Id:%s, track Id：%s, Kind:%s, tag:%s\n"), 
                utf2unicode(itor->GetUserId()).c_str(),
                utf2unicode(itor->GetTrackId()).c_str(),
                utf2unicode(itor->GetKind()).c_str(),
                utf2unicode(itor->GetTag()).c_str()
                );
        }
    }
    if (0 == error_code_) {
        _snwprintf(
            buff, 
            1024, 
            _T("数据流订阅成功数量：%d，失败数量：%d"), 
            succ_num,
            failed_num
        );
    }
    _wnd_status_bar.SetText(buff, 1, 0);
    
    AdjustMergeStreamLayouts();

    for (auto&& itor : track_info_list_) {
        itor->Release();
    }
}

void CRtcDemoV2::OnRemoteAddTracks(const qiniu_v2::TrackInfoList& track_list_)
{
    ASSERT(!track_list_.empty());
    wchar_t buff[1024] = { 0 };
    _snwprintf(buff, 1024, _T("%s 发布了 %d 路媒体流。"), 
        utf2unicode(track_list_.front()->GetUserId()).c_str(), track_list_.size());
    _wnd_status_bar.SetText(buff, 1, 0);

    lock_guard<recursive_mutex> lck(_mutex);
    qiniu_v2::TrackInfoList sub_tracks_list;
    for (auto&& itor : track_list_) {
        TRACE("%s", itor->GetKind());
        auto tmp_track_ptr = qiniu_v2::QNTrackInfo::Copy(itor);
        shared_ptr<TrackInfoUI> tiu(new TrackInfoUI(this, tmp_track_ptr));
        if (tiu->render_wnd_ptr) {
            tmp_track_ptr->SetRenderHwnd((void*)tiu->render_wnd_ptr->m_hWnd);
        }
        _remote_tracks_map.insert_or_assign(tmp_track_ptr->GetTrackId(), tiu);

        sub_tracks_list.emplace_back(tmp_track_ptr);
    }
    _rtc_room_interface->SubscribeTracks(sub_tracks_list);
    // 调整订阅窗口的布局
    AdjustSubscribeLayouts();

    for (auto&& itor : track_list_) {
        itor->Release();
    }
}

void CRtcDemoV2::OnRemoteDeleteTracks(const list<string>& track_list_)
{
    wchar_t buff[1024] = { 0 };
    _snwprintf(buff, 1024, _T("远端用户取消发布了 %d 路媒体流。"), track_list_.size());
    _wnd_status_bar.SetText(buff, 1, 0);

    // 释放本地资源
    if (_remote_tracks_map.empty()) {
        return;
    }
    lock_guard<recursive_mutex> lck(_mutex);
    for (auto&& itor : track_list_) {
        if (_remote_tracks_map.empty()) {
            break;
        }
        auto tmp_ptr = _remote_tracks_map.begin();
        while (tmp_ptr != _remote_tracks_map.end()) {
            if (tmp_ptr->second->track_info_ptr->GetTrackId().compare(itor) == 0) {
                _remote_tracks_map.erase(tmp_ptr);
                break;
            }
            ++tmp_ptr;
        }
    }
    AdjustSubscribeLayouts();
}

void CRtcDemoV2::OnRemoteUserJoin(const string& user_id_, const string& user_data_)
{
    lock_guard<recursive_mutex> lck(_mutex);

    _user_list.push_back(user_id_);

    CString str;
    for (int i = 0; i < _user_list_ctrl.GetItemCount(); i++) {
        str = _user_list_ctrl.GetItemText(i, 0);
        if (str.CompareNoCase(utf2unicode(user_id_).c_str()) == 0) {
            _user_list_ctrl.DeleteItem(i);
            break;
        }
    }
    _user_list_ctrl.InsertItem(0, utf2unicode(user_id_).c_str());
    _user_list_ctrl.SetItemText(0, 1, _T(""));

    wchar_t buff[1024] = { 0 };
    _snwprintf(
        buff,
        1024,
        _T("%s 加入了房间！"),
        utf2unicode(user_id_).c_str()
    );
    _wnd_status_bar.SetText(buff, 1, 0);
}

void CRtcDemoV2::OnRemoteUserLeave(const string& user_id_, int error_code_)
{
    lock_guard<recursive_mutex> lck(_mutex);
    CString str;
    for (int i = 0; i < _user_list_ctrl.GetItemCount(); i++) {
        str = _user_list_ctrl.GetItemText(i, 0);
        if (str.CompareNoCase(utf2unicode(user_id_).c_str()) == 0) {
            _user_list_ctrl.DeleteItem(i);
            break;
        }
    }

    auto itor = std::find(_user_list.begin(), _user_list.end(), user_id_);
    if (itor != _user_list.end()) {
        _user_list.erase(itor);
    }

    wchar_t buff[1024] = { 0 };
    _snwprintf(
        buff,
        1024,
        _T("%s 离开了房间！"),
        utf2unicode(user_id_).c_str()
    );
    _wnd_status_bar.SetText(buff, 1, 0);
}

void CRtcDemoV2::OnKickoutResult(const std::string& kicked_out_user_id_, 
    int error_code_, const std::string& error_str_)
{
    lock_guard<recursive_mutex> lck(_mutex);

    auto itor = std::find(_user_list.begin(), _user_list.end(), kicked_out_user_id_);
    if (itor != _user_list.end()) {
        _user_list.erase(itor);
    }

    wchar_t buff[1024] = { 0 };
    if (0 == error_code_) {
        _snwprintf(
            buff,
            1024,
            _T("踢出用户：%s 成功！"),
            utf2unicode(kicked_out_user_id_).c_str()
        );
    } else {
        _snwprintf(
            buff,
            1024,
            _T("踢出用户：%s 失败！error code:%d, error msg:%s"),
            utf2unicode(kicked_out_user_id_).c_str(),
            error_code_,
            utf2unicode(error_str_).c_str()
        );
    }
    _wnd_status_bar.SetText(buff, 1, 0);
}

void CRtcDemoV2::OnRemoteTrackMuted(const string& track_id_, bool mute_flag_)
{
    wchar_t buff[1024] = { 0 };
    _snwprintf(
        buff,
        1024,
        mute_flag_ ? _T("远端用户静默了 Track Id : %s") : _T("远端用户取消了静默 Track Id : %s"),
        utf2unicode(track_id_).c_str()
    );
    _wnd_status_bar.SetText(buff, 1, 0);
}

void CRtcDemoV2::OnStatisticsUpdated(const qiniu_v2::StatisticsReport& statistics_)
{
    wchar_t dest_buf[1024] = { 0 };
    if (statistics_.is_video) {
        _snwprintf(dest_buf,
            sizeof(dest_buf),
            _T("用户:%s, Track Id:%s 视频: 分辨率:%d*%d, 帧率:%d, 码率:%d kbps, 丢包率:%0.3f"),
            utf2unicode(statistics_.user_id).c_str(),
            utf2unicode(statistics_.track_id).c_str(),
            statistics_.video_width,
            statistics_.video_height,
            statistics_.video_frame_rate,
            statistics_.video_bitrate / 1024,
            statistics_.video_packet_lost_rate
        );
    } else {
        _snwprintf(dest_buf,
            sizeof(dest_buf),
            _T("用户:%s, Track Id:%s, 音频：码率:%d kbps, 丢包率:%0.3f"),
            utf2unicode(statistics_.user_id).c_str(),
            utf2unicode(statistics_.track_id).c_str(),
            statistics_.audio_bitrate / 1024,
            statistics_.audio_packet_lost_rate
        );
    }
    //TRACE(utf2unicode(dest_buf).c_str());
    TRACE(dest_buf);

    int line_count = _msg_rich_edit_ctrl.GetLineCount();
    if (line_count >= 1000) {
        // 此控件可存储数据量有限，为避免卡顿，及时清除
        _msg_rich_edit_ctrl.SetWindowTextW(_T(""));
        _msg_rich_edit_ctrl.UpdateData();
        _msg_rich_edit_ctrl.Invalidate();
    }
    _msg_rich_edit_ctrl.SetSel(-1, -1);
    _msg_rich_edit_ctrl.ReplaceSel(_T("\n"));
    _msg_rich_edit_ctrl.ReplaceSel(dest_buf);
    _msg_rich_edit_ctrl.PostMessage(WM_VSCROLL, SB_BOTTOM, 0);
}

void CRtcDemoV2::OnAudioPCMFrame(const unsigned char* audio_data_, 
    int bits_per_sample_, int sample_rate_, size_t number_of_channels_, 
    size_t number_of_frames_, const std::string& user_id_)
{
    if (bits_per_sample_ / 8 == sizeof(int16_t)) {
        //ASSERT(bits_per_sample_ / 8 == sizeof(int16_t));
        // 可以借助以下代码计算音量的实时分贝值
        auto level = ProcessAudioLevel(
            (int16_t*)audio_data_,
            bits_per_sample_ / 8 * number_of_channels_ * number_of_frames_ / sizeof(int16_t)
        );
    }
}

void CRtcDemoV2::OnAudioDeviceStateChanged(
    qiniu_v2::AudioDeviceState device_state_, const std::string& device_guid_)
{
    thread(
        [&, device_state_, device_guid_] {
        wchar_t buf[512] = { 0 };
        if (qiniu_v2::ads_active != device_state_) {
            _snwprintf(
                buf,
                512,
                _T("音频设备：%s 被拔出！"),
                utf2unicode(device_guid_).c_str()
            );
            AfxMessageBox(buf, MB_OK);
        }
    }
    ).detach();
}

void CRtcDemoV2::OnVideoDeviceStateChanged(
    qiniu_v2::VideoDeviceState device_state_, const std::string& device_name_)
{
    thread(
        [&, device_state_, device_name_] {
        wchar_t buf[512] = { 0 };
        if (qiniu_v2::vds_lost == device_state_) {
            _snwprintf(
                buf,
                512,
                _T("视频设备：%s 被拔出！"),
                utf2unicode(device_name_).c_str()
            );
            AfxMessageBox(buf, MB_OK);
        }
    }
    ).detach();
}

void CRtcDemoV2::OnVideoFrame(const unsigned char* raw_data_, int data_len_, 
    int width_, int height_, qiniu_v2::VideoCaptureType video_type_, 
    const std::string& track_id_, const std::string& user_id_)
{

}

void CRtcDemoV2::OnVideoFramePreview(const unsigned char* raw_data_, int data_len_,
    int width_, int height_, qiniu_v2::VideoCaptureType video_type_)
{

}

void CRtcDemoV2::OnBnClickedButtonLogin()
{
    // TODO: Add your control notification handler code here
    CString btn_str;
    GetDlgItemText(IDC_BUTTON_LOGIN, btn_str);
    if (btn_str.CompareNoCase(_T("登录")) == 0) {
        GetDlgItemText(IDC_EDIT_APPID, _app_id);
        GetDlgItemText(IDC_EDIT_ROOM_ID, _room_name);
        GetDlgItemText(IDC_EDIT_PLAYER_ID, _user_id);
        if (_room_name.IsEmpty() || _user_id.IsEmpty()) {
            MessageBox(_T("Room ID and Player ID can't be NULL!"));
            return;
        }
        WriteConfigFile();

        GetDlgItem(IDC_BUTTON_LOGIN)->SetWindowText(_T("登录中"));
        GetDlgItem(IDC_BUTTON_LOGIN)->EnableWindow(FALSE);

        //向 AppServer 获取 token 
        _room_token.clear();
        int ret = GetRoomToken(
            unicode2utf(_app_id.GetBuffer()),
            unicode2utf(_room_name.GetBuffer()),
            unicode2utf(_user_id.GetBuffer()), _room_token);
        if (ret != 0) {
            CString msg_str;
            msg_str.Format(_T("获取房间 token 失败，请检查您的网络是否正常！Err:%d"), ret);
            _wnd_status_bar.SetText(msg_str.GetBuffer(), 1, 0);
            //MessageBox(_T("获取房间 token 失败，请检查您的网络是否正常！"));
            GetDlgItem(IDC_BUTTON_LOGIN)->SetWindowText(_T("登录"));
            GetDlgItem(IDC_BUTTON_LOGIN)->EnableWindow(TRUE);

            thread([=] {
                  Sleep(2000);
                  PostMessage(WM_COMMAND, MAKEWPARAM(IDC_BUTTON_LOGIN, BN_CLICKED), NULL);
              }).detach();
            return;
        }
        if (_strnicmp(
            const_cast<char*>(unicode2utf(_user_id.GetBuffer()).c_str()),
            "admin",
            unicode2utf(_user_id.GetBuffer()).length()) == 0) {
            _contain_admin_flag = true;
        } else {
            _contain_admin_flag = false;
        }

        _wnd_status_bar.SetText(_T("获取房间 token 成功！"), 1, 0);
        _rtc_room_interface->JoinRoom(_room_token);
    } else {
        // 退出房间前，发布停止合流的命令
        if (_contain_admin_flag) {
            _rtc_room_interface->StopMergeStream();
        }
        if (1 == ((CButton*)GetDlgItem(IDC_CHECK_MERGE))->GetCheck()) {
            _rtc_room_interface->StopMergeStream(_custom_merge_id);
        }
        _rtc_room_interface->LeaveRoom();

        SetDlgItemText(IDC_BUTTON_LOGIN, _T("登录"));
        _wnd_status_bar.SetText(_T("当前未登录房间！"), 1, 0);
        _user_list_ctrl.DeleteAllItems();
        ((CButton*)GetDlgItem(IDC_CHECK_MUTE_AUDIO))->SetCheck(0);
        ((CButton*)GetDlgItem(IDC_CHECK_MUTE_VIDEO))->SetCheck(0);

        _remote_tracks_map.clear();

        KillTimer(UPDATE_TIME_DURATION_TIMER);
        Invalidate();
    }
}

void CRtcDemoV2::OnBnClickedButtonPreviewVideo()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_video_interface) {
        return;
    }
    // 首先获取选中设备 name 和 id
    CString cur_dev_name;
    string cur_dev_id;
    GetDlgItem(IDC_COMBO_CAMERA)->GetWindowTextW(cur_dev_name);
    if (cur_dev_name.IsEmpty()) {
        MessageBox(_T("您当前没有任何视频设备！"));
        return;
    }
    // 获取 device id
    auto itor = _camera_dev_map.begin();
    while (itor != _camera_dev_map.end()) {
        if (itor->second.device_name.compare(unicode2utf(cur_dev_name.GetBuffer())) == 0) {
            cur_dev_id = itor->first;
            break;
        }
        ++itor;
    }

    CString btn_text;
    GetDlgItemText(IDC_BUTTON_PREVIEW_VIDEO, btn_text);
    if (btn_text.CompareNoCase(_T("取消预览")) == 0) {
        _rtc_video_interface->UnPreviewCamera(cur_dev_id);
        GetDlgItem(IDC_COMBO_CAMERA)->EnableWindow(TRUE);
        SetDlgItemText(IDC_BUTTON_PREVIEW_VIDEO, _T("预览"));
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW2)->Invalidate();
        return;
    }

    qiniu_v2::CameraSetting camera_setting;
    camera_setting.device_name = unicode2utf(cur_dev_name.GetBuffer());
    camera_setting.device_id   = cur_dev_id;
    camera_setting.width       = 640;
    camera_setting.height      = 480;
    camera_setting.max_fps     = 15;
    camera_setting.render_hwnd = GetDlgItem(IDC_STATIC_VIDEO_PREVIEW2)->m_hWnd;

    if (0 != _rtc_video_interface->PreviewCamera(camera_setting)) {
        MessageBox(_T("预览失败！"));
    } else {
        SetDlgItemText(IDC_BUTTON_PREVIEW_VIDEO, _T("取消预览"));
        GetDlgItem(IDC_COMBO_CAMERA)->EnableWindow(FALSE);
    }
}

void CRtcDemoV2::OnBnClickedBtnFlush()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_video_interface) {
        return;
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_SCREEN))->ResetContent();
    int screen_count = _rtc_video_interface->GetScreenWindowCount();
    for (int i(0); i < screen_count; ++i) {
        qiniu_v2::ScreenWindowInfo sw = _rtc_video_interface->GetScreenWindowInfo(i);
        _screen_info_map[sw.id] = sw;
        ((CComboBox *)GetDlgItem(IDC_COMBO_SCREEN))->InsertString(-1, utf2unicode(sw.title).c_str());
    }
    ((CComboBox *)GetDlgItem(IDC_COMBO_SCREEN))->SetCurSel(0);
}

void CRtcDemoV2::OnBnClickedButtonPreviewScreen()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_video_interface) {
        return;
    }
    // 首先获取选中设备 name 和 id
    CString cur_screen_title;
    GetDlgItem(IDC_COMBO_SCREEN)->GetWindowTextW(cur_screen_title);

    // 获取 source id
    int source_id(-1);
    auto itor = _screen_info_map.begin();
    while (itor != _screen_info_map.end()) {
        if (itor->second.title.compare(unicode2utf(cur_screen_title.GetBuffer())) == 0) {
            source_id = itor->first;
            break;
        }
        ++itor;
    }

    CString btn_text;
    GetDlgItemText(IDC_BUTTON_PREVIEW_SCREEN, btn_text);
    if (btn_text.CompareNoCase(_T("取消预览")) == 0) {
        _rtc_video_interface->UnPreviewScreenSource(source_id);
        GetDlgItem(IDC_COMBO_SCREEN)->EnableWindow(TRUE);
        GetDlgItem(IDC_BTN_FLUSH)->EnableWindow(TRUE);
        SetDlgItemText(IDC_BUTTON_PREVIEW_SCREEN, _T("预览屏幕"));
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW)->Invalidate();
        return;
    }

    if (0 != _rtc_video_interface->PreviewScreenSource(
        source_id, 
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW4)->m_hWnd, 
        true) ) {
        MessageBox(_T("预览失败！"));
    } else {
        SetDlgItemText(IDC_BUTTON_PREVIEW_SCREEN, _T("取消预览"));
        GetDlgItem(IDC_COMBO_SCREEN)->EnableWindow(FALSE);
        GetDlgItem(IDC_BTN_FLUSH)->EnableWindow(FALSE);
    }
}

void CRtcDemoV2::StartPublish()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    qiniu_v2::TrackInfoList track_list;

CHECK_CAMERA:
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->GetCheck()) {
        CString video_dev_name;
        string video_dev_id;
        int audio_recorder_device_index(-1);

        GetDlgItem(IDC_COMBO_CAMERA)->GetWindowTextW(video_dev_name);
        if (video_dev_name.IsEmpty()) {
            thread([] {
                AfxMessageBox(_T("您当前没有任何视频设备！"));
            }).detach();
            goto CHECK_SCREEN;
        }
        auto itor = _camera_dev_map.begin();
        while (itor != _camera_dev_map.end()) {
            if (itor->second.device_name.compare(unicode2utf(video_dev_name.GetBuffer())) == 0) {
                video_dev_id = itor->first;
                break;
            }
            ++itor;
        }
        auto camera_size = FindBestVideoSize(_camera_dev_map[video_dev_id].capability_vec);
        auto video_track_ptr = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            video_dev_id,
            CAMERA_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW)->m_hWnd,
            std::get<0>(camera_size),
            std::get<1>(camera_size),
            15,
            500000,
            qiniu_v2::tst_Camera,
            false
        );
        track_list.emplace_back(video_track_ptr);
    }

CHECK_SCREEN:
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->GetCheck()) {
        CString wnd_title;
        int source_id(-1);
        GetDlgItem(IDC_COMBO_SCREEN)->GetWindowTextW(wnd_title);
        for (auto&& itor : _screen_info_map) {
            if (itor.second.title.compare(unicode2utf(wnd_title.GetBuffer())) == 0) {
                source_id = itor.first;
                break;
            }
        }
        if (-1 == source_id) {
            goto CHECK_AUDIO;
        }
        auto video_track_ptr = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            std::to_string(source_id),
            SCREENCASTS_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW2)->m_hWnd,
            640,
            480,
            30,
            500000,
            qiniu_v2::tst_ScreenCasts,
            false
        );
        track_list.emplace_back(video_track_ptr);
    }
CHECK_AUDIO:
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->GetCheck()) {
        auto audio_track = qiniu_v2::QNTrackInfo::CreateAudioTrackInfo(
            MICROPHONE_TAG,
            32000,
            false
        );
        track_list.emplace_back(audio_track);
    }
CHECK_EXTERNAL:
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->GetCheck()) {
        auto video_track_ptr = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            "",
            EXTERNAL_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW3)->m_hWnd,
            426,
            240,
            30,
            300000,
            qiniu_v2::tst_ExternalYUV,
            false
        );
        track_list.emplace_back(video_track_ptr);
    }

    if (!track_list.empty()) {
        auto ret = _rtc_room_interface->PublishTracks(track_list);
        if (ret == Err_Tracks_Publish_All_Failed) {
            thread([]() {
                AfxMessageBox(_T("全部发布失败，请检查您的网络状态！"));
            }).detach();
        } else if (ret == Err_Tracks_Publish_All_Failed) {
            thread([]() {
                AfxMessageBox(_T("部分发布失败，请检查您的设备状态是否可用？"));
            }).detach();
        }
        qiniu_v2::QNTrackInfo::ReleaseList(track_list);
    }
}

void CRtcDemoV2::StopPublish()
{
    if (!_rtc_room_interface) {
        return;
    }
    lock_guard<recursive_mutex> lck(_mutex);
    list<string> track_list;
    for (auto&& itor : _local_tracks_list) {
        track_list.emplace_back(itor->GetTrackId());
    }
    _rtc_room_interface->UnPublishTracks(track_list);

    qiniu_v2::QNTrackInfo::ReleaseList(_local_tracks_list);
}

void CRtcDemoV2::OnBnClickedCheckCamera()
{
    if (!_rtc_room_interface) {
        return;
    }
    lock_guard<recursive_mutex> lck(_mutex);
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->GetCheck()) {
        if (!_rtc_room_interface->IsJoined()) {
            return;
        }
        CString video_dev_name;
        string video_dev_id;
        int audio_recorder_device_index(-1);

        GetDlgItem(IDC_COMBO_CAMERA)->GetWindowTextW(video_dev_name);
        if (video_dev_name.IsEmpty()) {
            thread([] {
                AfxMessageBox(_T("您当前没有任何视频设备！"));
            }).detach();
            return;
        }
        auto itor = _camera_dev_map.begin();
        while (itor != _camera_dev_map.end()) {
            if (itor->second.device_name.compare(unicode2utf(video_dev_name.GetBuffer())) == 0) {
                video_dev_id.assign(itor->first.c_str(), itor->first.length());
                TRACE("video dev id ptr:%x, ptr2:%x ;", itor->first.c_str(), video_dev_id.c_str());
                break;
            }
            ++itor;
        }
        auto camera_size = FindBestVideoSize(_camera_dev_map[video_dev_id].capability_vec);
        auto video_track_ptr = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            video_dev_id,
            CAMERA_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW)->m_hWnd,
            std::get<0>(camera_size),
            std::get<1>(camera_size),
            15,
            500000,
            qiniu_v2::tst_Camera,
            false
        );
        qiniu_v2::TrackInfoList track_list;
        track_list.push_back(video_track_ptr);
        auto ret = _rtc_room_interface->PublishTracks(track_list);
        if (ret != 0) {
            ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->SetCheck(0);
            MessageBox(_T("发布失败，请检查设备状态是否可用？或者不要操作太快！"));
        } else {
            // 临时禁用，待发布结果通知后再激活控件，避免频繁操作
            ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->EnableWindow(FALSE);
        }
        video_track_ptr->Release();
    } else {
        list<string> track_list;
        if (_local_tracks_list.empty()) {
            if (_rtc_room_interface->IsJoined()) {
                ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->SetCheck(1);
            }
            return;
        }
        auto itor = _local_tracks_list.begin();
        while (itor != _local_tracks_list.end()) {
            if ((*itor)->GetTag().compare(CAMERA_TAG) == 0) {
                track_list.emplace_back((*itor)->GetTrackId());
                if (Err_Pre_Publish_Not_Complete == _rtc_room_interface->UnPublishTracks(track_list)) {
                    ((CButton*)GetDlgItem(IDC_CHECK_CAMERA))->SetCheck(1);
                }
                (*itor)->Release();
                _local_tracks_list.erase(itor);
                break;
            }
            ++itor;
        }
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW)->Invalidate();
        _wnd_status_bar.SetText(_T("取消发布摄像头"), 1, 0);
    }
}

void CRtcDemoV2::OnBnClickedCheckScreen()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    lock_guard<recursive_mutex> lck(_mutex);
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->GetCheck()) {
        if (!_rtc_room_interface->IsJoined()) {
            return;
        }
        CString wnd_title;
        int source_id(-1);
        GetDlgItem(IDC_COMBO_SCREEN)->GetWindowTextW(wnd_title);
        for (auto&& itor : _screen_info_map) {
            if (itor.second.title.compare(unicode2utf(wnd_title.GetBuffer())) == 0) {
                source_id = itor.first;
                break;
            }
        }
        if (-1 == source_id) {
            ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->SetCheck(0);
            return;
        }
        auto video_track_ptr2 = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            std::to_string(source_id),
            SCREENCASTS_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW2)->m_hWnd,
            640,
            480,
            30,
            300000,
            qiniu_v2::tst_ScreenCasts,
            false
        );
        qiniu_v2::TrackInfoList track_list;
        track_list.emplace_back(video_track_ptr2);
        auto ret = _rtc_room_interface->PublishTracks(track_list);
        if (ret != 0) {
            ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->SetCheck(0);
            MessageBox(_T("发布失败，请检查设备状态是否可用？或者不要操作太快！"));
        } else {
            // 临时禁用，待发布结果通知后再激活控件，避免频繁操作
            ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->EnableWindow(FALSE);
        }
        video_track_ptr2->Release();
    } else {
        list<string> track_list;
        if (_local_tracks_list.empty()) {
            if (_rtc_room_interface->IsJoined()) {
                ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->SetCheck(1);
            }
            return;
        }
        auto itor = _local_tracks_list.begin();
        while (itor != _local_tracks_list.end())
        {
            if ((*itor)->GetTag().compare(SCREENCASTS_TAG) == 0) {
                track_list.emplace_back((*itor)->GetTrackId());
                if (Err_Pre_Publish_Not_Complete == _rtc_room_interface->UnPublishTracks(track_list)) {
                    ((CButton*)GetDlgItem(IDC_CHECK_SCREEN))->SetCheck(1);
                }
                (*itor)->Release();
                _local_tracks_list.erase(itor);
                break;
            }
            ++itor;
        }
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW2)->Invalidate();
        _wnd_status_bar.SetText(_T("取消发布屏幕分享"), 1, 0);
    }
}


void CRtcDemoV2::OnBnClickedCheckAudio()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    ((CButton*)GetDlgItem(IDC_CHECK_MUTE_AUDIO))->SetCheck(0);

    lock_guard<recursive_mutex> lck(_mutex);
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->GetCheck()) {
        if (!_rtc_room_interface->IsJoined()) {
            return;
        }

        OnCbnSelchangeComboMicrophone();

        auto audio_track = qiniu_v2::QNTrackInfo::CreateAudioTrackInfo(
            MICROPHONE_TAG,
            32000,
            false
        );
        qiniu_v2::TrackInfoList track_list;
        track_list.emplace_back(audio_track);
        auto ret = _rtc_room_interface->PublishTracks(track_list);
        if (ret != 0) {
            ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->SetCheck(0);
            MessageBox(_T("发布失败，请检查设备状态是否可用？或者不要操作太快！"));
        } else {
            // 临时禁用，待发布结果通知后再激活控件，避免频繁操作
            ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->EnableWindow(FALSE);
        }
        audio_track->Release();
    } else {
        list<string> track_list;
        if (_local_tracks_list.empty()) {
            if (_rtc_room_interface->IsJoined()) {
                ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->SetCheck(1);
            }
            return;
        }
        auto itor = _local_tracks_list.begin();
        while (itor != _local_tracks_list.end()) {
            if ((*itor)->GetTag().compare(MICROPHONE_TAG) == 0) {
                track_list.emplace_back((*itor)->GetTrackId());
                if (Err_Pre_Publish_Not_Complete == _rtc_room_interface->UnPublishTracks(track_list)) {
                    ((CButton*)GetDlgItem(IDC_CHECK_AUDIO))->SetCheck(1);
                }

                (*itor)->Release();
                _local_tracks_list.erase(itor);
                break;
            }
            ++itor;
        }
        _wnd_status_bar.SetText(_T("取消发布音频"), 1, 0);
    }
}

void CRtcDemoV2::OnBnClickedCheckImportRawData()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    lock_guard<recursive_mutex> lck(_mutex);
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->GetCheck()) {
        if (!_rtc_room_interface->IsJoined()) {
            return;
        }
        _rtc_audio_interface->EnableAudioFakeInput(true);

        auto video_track_ptr = qiniu_v2::QNTrackInfo::CreateVideoTrackInfo(
            "",
            EXTERNAL_TAG,
            GetDlgItem(IDC_STATIC_VIDEO_PREVIEW3)->m_hWnd,
            426,
            240,
            30,
            300000,
            qiniu_v2::tst_ExternalYUV,
            false
        );
        qiniu_v2::TrackInfoList track_list;
        track_list.emplace_back(video_track_ptr);
        auto ret = _rtc_room_interface->PublishTracks(track_list);
        if (ret != 0) {
            ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->SetCheck(0);
            MessageBox(_T("发布失败，请检查设备状态是否可用？或者不要操作太快！"));
        } else {
            // 临时禁用，待发布结果通知后再激活控件，避免频繁操作
            ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->EnableWindow(FALSE);
        }
        video_track_ptr->Release();
    } else {
        _rtc_audio_interface->EnableAudioFakeInput(false);

        list<string> track_list;
        if (_local_tracks_list.empty()) {
            if (_rtc_room_interface->IsJoined()) {
                ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->SetCheck(1);
            }
            return;
        }
        auto itor = _local_tracks_list.begin();
        while (itor != _local_tracks_list.end()) {
            if ((*itor)->GetTag().compare(EXTERNAL_TAG) == 0) {
                track_list.emplace_back((*itor)->GetTrackId());
                if (Err_Pre_Publish_Not_Complete == _rtc_room_interface->UnPublishTracks(track_list)) {
                    ((CButton*)GetDlgItem(IDC_CHECK_IMPORT_RAW_DATA))->SetCheck(1);
                }
                (*itor)->Release();
                _local_tracks_list.erase(itor);
                break;
            }
            ++itor;
        }
        GetDlgItem(IDC_STATIC_VIDEO_PREVIEW3)->Invalidate();

        _stop_external_flag = true;
        if (_fake_audio_thread.joinable()) {
            _fake_audio_thread.join();
        }
        if (_fake_video_thread.joinable()) {
            _fake_video_thread.join();
        }
        _wnd_status_bar.SetText(_T("取消发布外部导入"), 1, 0);
        AdjustMergeStreamLayouts();
    }
}

void CRtcDemoV2::ImportExternalRawFrame(const string& track_id_)
{
    // 模拟导入视频数据,当前使用当前目录下指定的音视频文件
    _stop_external_flag = true;
    if (_fake_video_thread.joinable()) {
        _fake_video_thread.join();
    }
    if (_fake_audio_thread.joinable()) {
        _fake_audio_thread.join();
    }

    string track_id(track_id_.c_str());
    _fake_video_thread = thread([&, track_id] {
        FILE* fp = nullptr;
        fopen_s(&fp, "426x240.yuv", "rb");
        uint8_t *buf = (uint8_t*)malloc(426 * 240 * 3 / 2);
        if (!fp || !buf) {
            MessageBox(_T("foreman_320x240.yuv 文件打开失败，请确认此文件件是否存在!"));
            return;
        }
        size_t ret(0);
        _stop_external_flag = false;
        chrono::system_clock::time_point start_tp = chrono::system_clock::now();
        while (!_stop_external_flag) {
            ret = fread_s(buf, 426 * 240 * 3 / 2, 1, 426 * 240 * 3 / 2, fp);
            if (ret > 0) {
                _rtc_video_interface->InputVideoFrame(
                    track_id,
                    buf,
                    426 * 240 * 3 / 2,
                    426,
                    240,
                    chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now() - start_tp).count(),
                    qiniu_v2::VideoCaptureType::kI420,
                    qiniu_v2::kVideoRotation_0);
            } else {
                fseek(fp, 0, SEEK_SET);
                continue;
            }
            Sleep(1000 / 30);
        }
        free(buf);
        fclose(fp);
    });

    // 模拟导入音频数据
    _rtc_audio_interface->EnableAudioFakeInput(true);
    _fake_audio_thread = thread([&] {
        FILE* fp = nullptr;
        fopen_s(&fp, "44100_16bits_2channels.pcm", "rb");
        if (!fp) {
            MessageBox(_T("PCM 文件:44100_16bits_2channels.pcm 打开失败，请确认此文件件是否存在!"));
            return;
        }
        // 每次导入 20 ms 的数据，即 441 * 2 个 samples
        uint8_t *buf = (uint8_t*)malloc(441 * 2 * 2 * 2);

        size_t ret(0);
        chrono::system_clock::time_point start_tp = chrono::system_clock::now();
        int64_t audio_frame_count(0);
        while (!_stop_external_flag) {
            if (chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now() - start_tp).count() >= audio_frame_count * 20000) {
            } else {
                Sleep(10);
                continue;
            }

            ret = fread_s(buf, 441 * 2 * 4, 1, 441 * 2 * 4, fp);
            if (ret >= 441 * 8) {
                _rtc_audio_interface->InputAudioFrame(
                    buf,
                    441 * 8,
                    16,
                    44100,
                    2,
                    441 * 2
                );
                ++audio_frame_count;
            } else {
                fseek(fp, 0, SEEK_SET);
                continue;
            }
        }
        free(buf);
        fclose(fp);
    });
}

void CRtcDemoV2::CreateCustomMergeJob()
{
    qiniu_v2::MergeJob job_desc;
    job_desc.job_id         = unicode2utf(_room_name.GetBuffer()) + "_merge";
    job_desc.publish_url    = _merge_config.publish_url;
    job_desc.width          = _merge_config.job_width;
    job_desc.height         = _merge_config.job_height;
    job_desc.fps            = _merge_config.job_fps;
    job_desc.bitrate        = _merge_config.job_bitrate;
    job_desc.min_bitrate    = _merge_config.job_min_bitrate;
    job_desc.max_bitrate    = _merge_config.job_max_bitrate;
    job_desc.stretch_mode   = qiniu_v2::ASPECT_FILL;

    qiniu_v2::MergeLayer background;
    background.layer_url    = _merge_config.background_url;
    background.pos_x        = _merge_config.background_x;
    background.pos_y        = _merge_config.background_y;
    background.layer_width  = _merge_config.background_width;
    background.layer_height = _merge_config.background_height;

    qiniu_v2::MergeLayerList watermarks;
    qiniu_v2::MergeLayer mark;
    mark.layer_url      = _merge_config.watermark_url;
    mark.pos_x          = _merge_config.watermark_x;
    mark.pos_y          = _merge_config.watermark_y;
    mark.layer_width    = _merge_config.watermark_width;
    mark.layer_height   = _merge_config.watermark_height;
    watermarks.emplace_back(mark);

    _custom_merge_id = job_desc.job_id;
    _rtc_room_interface->CreateMergeJob(job_desc, background, watermarks);
}

void CRtcDemoV2::AdjustMergeStreamLayouts()
{
    if (_contain_admin_flag) {
        struct Pos
        {
            int pos_x = 0;
            int pos_y = 0;
        };
        Pos merge_opts[9];
        for (int i(0); i < 3; ++i) {
            for (int j(0); j < 3; ++j) {
                merge_opts[i * 3 + j] = {j * CANVAS_WIDTH / 3, i * CANVAS_HEIGHT / 3};
            }
        }
        qiniu_v2::MergeOptInfoList add_tracks_list;
        list<string> remove_tracks_list;
        int num(-1);
        for (auto&& itor : _local_tracks_list) {
            qiniu_v2::MergeOptInfo merge_opt;
            merge_opt.track_id = itor->GetTrackId();
            merge_opt.is_video = true;
            if (itor->GetKind().compare("audio") == 0) {
                merge_opt.is_video = false;
                add_tracks_list.emplace_back(merge_opt);
                continue;
            }
            // Demo 保证视频九宫格布局
            if (itor->IsMaster()) {
                merge_opt.pos_x = 0;
                merge_opt.pos_y = 0;
                merge_opt.pos_z = 0;
                merge_opt.width = CANVAS_WIDTH;
                merge_opt.height = CANVAS_HEIGHT;
            } else {
                ++num;
                merge_opt.pos_x = merge_opts[num].pos_x;
                merge_opt.pos_y = merge_opts[num].pos_y;
                merge_opt.pos_z = 1;
                merge_opt.width = CANVAS_WIDTH / 3;
                merge_opt.height = CANVAS_HEIGHT / 3;
            }
            add_tracks_list.emplace_back(merge_opt);
        }
        for (auto&& itor : _remote_tracks_map) {
            qiniu_v2::MergeOptInfo merge_opt;
            merge_opt.track_id = itor.second->track_info_ptr->GetTrackId();
            merge_opt.is_video = true;
            if (itor.second->track_info_ptr->GetKind().compare("audio") == 0) {
                merge_opt.is_video = false;
                add_tracks_list.emplace_back(merge_opt);
                continue;
            }
            // Demo 保证视频九宫格布局
            ++num;
            merge_opt.pos_x = merge_opts[num].pos_x;
            merge_opt.pos_y = merge_opts[num].pos_y;
            merge_opt.pos_z = 1;
            merge_opt.width = CANVAS_WIDTH / 3;
            merge_opt.height = CANVAS_HEIGHT / 3;
            add_tracks_list.emplace_back(merge_opt);
        }
        _rtc_room_interface->SetMergeStreamlayouts(add_tracks_list, remove_tracks_list);
    }


    // 自定义合流，根据界面配置使用一个本地或者远端track 
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_MERGE))->GetCheck()) {

        qiniu_v2::MergeOptInfoList add_tracks_list;
        list<string> remove_tracks_list;
        for (auto&& itor : _local_tracks_list) {
            qiniu_v2::MergeOptInfo merge_opt;
            merge_opt.track_id = itor->GetTrackId();
            merge_opt.is_video = true;
            if (itor->GetKind().compare("audio") == 0 && _merge_config.merge_local_audio) {
                merge_opt.is_video = false;
                add_tracks_list.emplace_back(merge_opt);
                continue;
            }

            if (_merge_config.merge_local_video) {
                merge_opt.pos_x = _merge_config.local_video_x;
                merge_opt.pos_y = _merge_config.local_video_y;
                merge_opt.pos_z = 1;
                merge_opt.width = _merge_config.local_video_width;
                merge_opt.height = _merge_config.local_video_height;
                add_tracks_list.emplace_back(merge_opt);
            }
        }

        for (auto&& itor : _remote_tracks_map) {
            qiniu_v2::MergeOptInfo merge_opt;
            merge_opt.track_id = itor.second->track_info_ptr->GetTrackId();
            merge_opt.is_video = true;
            if (itor.second->track_info_ptr->GetKind().compare("audio") == 0 && _merge_config.merge_remote_audio) {
                merge_opt.is_video = false;
                add_tracks_list.emplace_back(merge_opt);
                continue;
            }

            if (_merge_config.merge_remote_video) {
                merge_opt.pos_x = _merge_config.remote_video_x;
                merge_opt.pos_y = _merge_config.remote_video_y;
                merge_opt.pos_z = 1;
                merge_opt.width = _merge_config.remote_video_width;
                merge_opt.height = _merge_config.remote_video_height;
                add_tracks_list.emplace_back(merge_opt);
            }
        }
        _rtc_room_interface->SetMergeStreamlayouts(add_tracks_list, remove_tracks_list, _custom_merge_id);
    }
}

void CRtcDemoV2::AdjustSubscribeLayouts()
{
    if (_remote_tracks_map.empty()) {
        return;
    }
    int wnd_num(0);
    RECT wnd_rc;
    GetWindowRect(&wnd_rc);
    TRACE("MainDialog rect x:%d, y:%d, right:%d, botton:%d\n", 
        wnd_rc.left, 
        wnd_rc.top,
        wnd_rc.right,
        wnd_rc.bottom
        );
    int main_wnd_height = wnd_rc.bottom - wnd_rc.top;
    const int wnd_width = 320;
    const int wnd_height = 240;
    int start_x = wnd_rc.left - wnd_width;
    int start_y = wnd_rc.top;
    for (auto&& itor : _remote_tracks_map) {
        if (itor.second->render_wnd_ptr) {
            itor.second->render_wnd_ptr->MoveWindow(
                start_x,
                start_y + wnd_height * wnd_num,
                wnd_width,
                wnd_height
            );
            if (start_y + wnd_width * wnd_num >= main_wnd_height) {
                wnd_num = 0;
                start_x += wnd_width;
            } else {
                ++wnd_num;
            }
        }
    }
}

void CRtcDemoV2::OnBnClickedCheckDesktopAudio()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_audio_interface) {
        return;
    }
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_DESKTOP_AUDIO))->GetCheck()) {
        _rtc_audio_interface->MixDesktopAudio(true);
    } else {
        _rtc_audio_interface->MixDesktopAudio(false);
    }
}

void CRtcDemoV2::OnBnClickedBtnKickout()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface || !_rtc_room_interface->IsJoined()) {
        return;
    }
    if (!_contain_admin_flag) {
        MessageBox(_T("您当前没有踢人权限，请使用 admin 进行登录！"));
        return;
    } else {
        int index = _user_list_ctrl.GetSelectionMark();
        if (index == -1) {
            MessageBox(_T("请选中要踢出的用户！"));
            return;
        }
        //所选择的用户当前没有发布媒体流
        CString user_id = _user_list_ctrl.GetItemText(index, 0);

        if (_rtc_room_interface) {
            _rtc_room_interface->KickoutUser(unicode2utf(user_id.GetBuffer()).c_str());
        }
    }
}

void CRtcDemoV2::OnBnClickedCheckMuteAudio()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    // 静默本地音频 Track，一端仅有一路音频 Track
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_MUTE_AUDIO))->GetCheck()) {
        _rtc_room_interface->MuteAudio(true);
    } else {
        _rtc_room_interface->MuteAudio(false);
    }

}

void CRtcDemoV2::OnBnClickedCheckMuteVideo()
{
    // TODO: Add your control notification handler code here
    if (!_rtc_room_interface) {
        return;
    }
    bool mute_flag = false;
    if (1 == ((CButton*)GetDlgItem(IDC_CHECK_MUTE_VIDEO))->GetCheck()) {
        mute_flag = true;
    }
    // 静默或取消静默本地所有的视频 Track
    // 这里对所有的视频 Track 进行控制
    lock_guard<recursive_mutex> lck(_mutex);
    for (auto&& itor : _local_tracks_list) {
        if (itor->GetKind().compare("video") == 0) {
            _rtc_room_interface->MuteVideo(itor->GetTrackId(), mute_flag);
        }
    }
}


void CRtcDemoV2::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
    // TODO: Add your message handler code here and/or call default
    lock_guard<recursive_mutex> lck(_mutex);
    if (pScrollBar->GetDlgCtrlID() == IDC_SLIDER_RECORD) {
        int pos = ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_RECORD))->GetPos();
        if (_rtc_audio_interface) {
            // 调整 SDK 内部音量
            _rtc_audio_interface->SetAudioVolume(
                unicode2utf(_user_id.GetBuffer()),
                pos / 100.0f
            );
        }
    } else if (pScrollBar->GetDlgCtrlID() == IDC_SLIDER_PLAYOUT) {
        int pos = ((CSliderCtrl*)GetDlgItem(IDC_SLIDER_PLAYOUT))->GetPos();

        if (_rtc_audio_interface) {
            // 调整所有用户的音量（这里只是为了方便演示，实际可根据需要控制指定用户的音量）
            for (auto&& itor : _user_list) {
                if (itor.compare(unicode2utf(_user_id.GetBuffer())) != 0) {
                    _rtc_audio_interface->SetAudioVolume(
                        itor,
                        pos / 100.0f
                    );
                }
            }
        }
    }

    __super::OnHScroll(nSBCode, nPos, pScrollBar);
    __super::OnHScroll(nSBCode, nPos, pScrollBar);
}


void CRtcDemoV2::OnCbnSelchangeComboMicrophone()
{
    // 输入音频设备配置
    if (!_rtc_audio_interface) {
        return;
    }
    int audio_recorder_device_index(-1);
    audio_recorder_device_index = ((CComboBox *)GetDlgItem(IDC_COMBO_MICROPHONE))->GetCurSel();
    audio_recorder_device_index = (audio_recorder_device_index == CB_ERR) ? 0 : audio_recorder_device_index;

    if (audio_recorder_device_index >= 0) {
        qiniu_v2::AudioDeviceInfo dev_info = _microphone_dev_map[audio_recorder_device_index];

        qiniu_v2::AudioDeviceSetting audio_setting;
        audio_setting.device_index = dev_info.device_index;
        audio_setting.device_type = qiniu_v2::AudioDeviceSetting::wdt_DefaultDevice;
        _rtc_audio_interface->SetRecordingDevice(audio_setting);
    }
}

void CRtcDemoV2::OnCbnSelchangeComboPlayout()
{
    // 播放音频设备配置
    if (!_rtc_audio_interface) {
        return;
    }
    int audio_playout_device_index(-1);
    audio_playout_device_index = ((CComboBox *)GetDlgItem(IDC_COMBO_PLAYOUT))->GetCurSel();
    audio_playout_device_index = (audio_playout_device_index == CB_ERR) ? 0 : audio_playout_device_index;

    if (audio_playout_device_index >= 0) {
        qiniu_v2::AudioDeviceInfo dev_info = _playout_dev_map[audio_playout_device_index];
        
        qiniu_v2::AudioDeviceSetting audio_setting;
        audio_setting.device_index = dev_info.device_index;
        audio_setting.device_type = qiniu_v2::AudioDeviceSetting::wdt_DefaultDevice;
        _rtc_audio_interface->SetPlayoutDevice(audio_setting);
    }
}

afx_msg LRESULT CRtcDemoV2::OnHandleMessage(WPARAM wParam, LPARAM lParam)
{
    MergeDialog::MergeConfig *config = (MergeDialog::MergeConfig *)lParam;
    _merge_config = *config;
    return 0;
}

void CRtcDemoV2::OnBnClickedButtonMerge()
{
    MergeDialog dlgMerge;
    dlgMerge._merge_config = _merge_config;
    dlgMerge.DoModal();
}
