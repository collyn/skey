# Kế hoạch đưa Uinput vào tiến trình fcitx5

Mục tiêu: Uinput chạy bằng quyền user của fcitx5, tự sẵn sàng khi dùng SKey, không cần bật một system service. Uinput tiếp tục chỉ phát Backspace và Escape; engine vẫn chịu trách nhiệm commit chữ.

Kế hoạch dựa trên HEAD `4b1efe9`. Phần mã nguồn và đóng gói đã triển khai;
kết quả kiểm tra và phần desktop/distro còn chờ xác minh ở
[uinput-migration-validation.md](uinput-migration-validation.md).

1. **Chốt phạm vi và baseline**

   - Giữ nguyên thuật toán tiếng Việt, Telex/VNI, Auto routing, cấu hình output mode, menu và ý nghĩa `replaceUinputWithNative`.
   - Giữ nguyên sync-anchor, số Backspace, pacing theo app, EWMA, settle delay, safety timer và replay phím. Không đổi các hằng số timing trong đợt chuyển transport.
   - Giữ implementation XTest/Libei của Native. Uinput có interface vòng đời tương tự Native nhưng không cần tạo một framework injector chung hoặc refactor Native.
   - Baseline đã xác minh: 11/11 CTest ngoài sandbox. Các lỗi omnibox ban đầu do sandbox chặn `send()` bằng `EPERM`; fixture Office thiếu bước drain timer replay X11, đã sửa fixture với expected output giữ nguyên.
   - Giải thích và xử lý lỗi baseline trong thay đổi riêng trước khi dùng test đó làm điều kiện chấp nhận migration. Không sửa expected output để làm test xanh.

   Hoàn thành khi có baseline tái hiện được, danh sách case bảo vệ và build cũ để đối chiếu.

2. **Thêm `UinputInjector` độc lập, chưa nối vào engine**

   - Thêm `src/uinput_injector.h/.cpp`, với `start`, `ready`, `status`, `backspaces` và `cancel` tương tự `NativeInjector`.
   - Một instance thuộc `SKeyEngine`, sở hữu một fd và một virtual keyboard; không tạo thiết bị theo từng `SKeyState` hay mỗi lần focus.
   - Dùng worker để khởi tạo và phát batch. Không chạy `usleep` hoặc pacing trên luồng xử lý phím của fcitx5. Worker chỉ làm I/O thiết bị; không truy cập `InputContext`, buffer tiếng Việt hoặc commit chữ.
   - Giữ bộ phím Backspace/Escape và thứ tự press/SYN/release/SYN của server hiện tại. Gửi hết một batch theo đúng thứ tự; không trộn hai batch hoặc âm thầm ghi đè batch đang chờ.
   - `backspaces` trả thành công khi nhận trọn batch để phát, không mang nghĩa ứng dụng đã xóa chữ. Việc xác nhận vẫn qua loopback/sync-anchor của engine.
   - Có trạng thái chưa khởi tạo, đang khởi tạo, sẵn sàng và lỗi; retry có giới hạn tần suất khi thiết bị hoặc ACL xuất hiện muộn. Kiểm tra lỗi `open`, `ioctl`, `write`, kể cả ghi thiếu hoặc bị ngắt.
   - Generation/token hủy phải xác định đúng giao dịch và IC đã gửi. Một IC cũ không được hủy batch mới của IC khác. Khi dừng phải ngắt được thời gian chờ, kết thúc worker rồi hủy thiết bị; fd dùng close-on-exec.
   - Thêm test I/O giả lập để kiểm tra thứ tự phím, pacing, queue bận, lỗi trước/sau khi phát một phần, retry và shutdown. Unit test không mở `/dev/uinput` thật.

   Hoàn thành khi injector được test độc lập và engine hiện tại vẫn hoạt động như baseline.

3. **Nối Uinput mới tại biên transport nhỏ nhất**

   - Điểm thay chính: `SKeyState::sendBackspaceUinput` trong `src/engine.cpp`. Giữ tên hàm và tham số để tránh sửa hàng loạt call site.
   - Giữ nguyên phần tính pacing, thống kê và quan sát deletion. Chỉ thay đoạn connect/send socket bằng gọi `UinputInjector`; nhánh Native giữ cách chọn transport hiện tại.
   - Khởi tạo sớm khi addon được nạp và bảo đảm start idempotent khi activate. Kiểm tra sẵn sàng tương tự Native, bao gồm omnibox Surrounding Text dùng injected Backspace.
   - Rà từng call site gửi Backspace. Nếu bị từ chối trước khi phát phím thì không arm transaction hoặc timer sẽ commit replacement lên chữ chưa xóa. Nếu lỗi sau khi phát một phần, báo về main loop và giữ xử lý loopback muộn; không tự phát lại toàn batch.
   - Nối cancel ở đúng các điểm bỏ giao dịch đã có. Giữ nguyên các guard deactivate giả của Chromium/Firefox và nhánh chờ reactivate; không biến mọi deactivate thành cancel/reset. Hủy chỉ ngăn phần chưa gửi, phím đã vào kernel vẫn cần được xử lý bởi cơ chế loopback hiện tại.
   - Kiểm tra thứ tự hủy thành viên của `SKeyEngine`: injector phải còn sống trong lúc state có thể cần cancel. Không mở rộng phạm vi refactor ownership ngoài yêu cầu này.
   - Chuyển fixture đang ghi vào `uinputClientFd_` sang bộ thu request giả tại biên injector. Giữ nguyên expected text, số deletion, sync-anchor và kiểm tra queue trong các test engine.
   - Warmup phải được kiểm tra với thao tác chọn IM rồi gõ ngay. Thời gian chờ tạo thiết bị hiện có không được chuyển thành blocking wait hoặc bị bỏ tùy tiện; đo readiness của compositor trước khi điều chỉnh riêng.

   Hoàn thành khi engine regression đạt baseline, các case gửi thất bại không commit sai và hai IC không can thiệp batch của nhau.

4. **Cấp quyền thiết bị khi cài đặt**

   - Cài rule `70-skey-uinput.rules` cấp `uaccess` riêng cho `/dev/uinput`, trước `73-seat-late.rules`. Không thêm user desktop vào nhóm `input`.
   - Đảm bảo module uinput được nạp khi boot và khi cài lần đầu. Package hook reload/trigger rule để phiên desktop đang active dùng được mà không phải bật service hoặc đổi group thủ công.
   - Kiểm tra trên Debian/Ubuntu, Arch và RPM: rule thực sự tạo ACL, fcitx5 mở được thiết bị và first input hoạt động sau cài/nâng cấp. Không dùng riêng kết quả `test -w` làm bằng chứng hoạt động.
   - NixOS dùng module và udev package khai báo để cấp quyền; không dùng đường dẫn `/usr/bin/setfacl`. Giữ tương thích cấu hình `services.fcitx5-skey.users` cũ bằng cảnh báo deprecation rõ ràng khi đổi ý nghĩa/tính cần thiết của option.
   - Thiếu quyền hoặc thiết bị phải có trạng thái chẩn đoán rõ; xử lý phím chưa sẵn sàng theo cách Native, không tiêu thụ phím rồi bỏ mất và không tự đổi output mode.

   Hoàn thành khi cài đặt chuẩn trên phiên desktop local cho phép dùng Uinput mà không chạy lệnh setup service bổ sung.

5. **Migration và dọn service sau khi transport mới đã qua kiểm tra**

   - Bỏ build/install server, unit template, polkit và sysusers dành riêng cho Uinput trong `src/CMakeLists.txt` và `data/CMakeLists.txt`.
   - Cập nhật đồng bộ `scripts/skey-setup`, `build.sh`, GUI restart, Debian hooks, RPM spec, Arch install hooks và NixOS module. Giữ các thao tác systemd phục vụ restart phiên fcitx5 còn cần thiết.
   - Khi nâng cấp, stop/disable đúng các instance Uinput cũ trước khi loại bỏ unit. Dọn rule/ACL cũ do SKey sở hữu theo thứ tự an toàn, không thay thế toàn bộ ACL thiết bị và không ảnh hưởng rule của ứng dụng khác.
   - Không xóa sysuser còn có tiến trình sử dụng. Giữ thao tác migration idempotent và hỗ trợ upgrade, remove, purge, reinstall; xét cả rule cũ còn sót do cơ chế config của RPM.
   - Giữ bộ gõ và config cá nhân hiện có. Cập nhật README, hướng dẫn NixOS và `scripts/TESTING.md` theo đường chẩn đoán mới.
   - Chuẩn bị rollback bằng package/build cũ và thủ tục khôi phục quyền/service. Không thêm fallback tự động âm thầm sang server cũ trong runtime mới.

   Hoàn thành khi cài mới và nâng cấp đều không cần Uinput service, không còn đường start/restart service ngoài mã migration có chủ đích.

6. **Nghiệm thu theo hành vi người dùng**

   | Nhóm | Kiểm tra bắt buộc |
   | --- | --- |
   | Engine | VietnameseTyping, EnginePerformance, InjectedBackspaceContract, các test omnibox X11/Wayland, WebKitSurroundingText và focus/cell tracker liên quan; broad CTest sau khi tích hợp xong. |
   | Injector | N Backspace và một anchor theo request engine; Escape; pace mặc định và override; queue bận; thiếu quyền; lỗi ghi một phần; cancel; shutdown; hai IC. |
   | Focus | Deactivate giả, focus mất thật, đổi ô Sheets, chuyển mode giữa giao dịch, tắt IM và hủy IC; không commit vào focus mới. |
   | Desktop thật | X11 và Wayland: terminal, Chromium page/omnibox, Firefox, Electron/VS Code, LibreOffice và Sheets/Docs. Kiểm tra chữ trong ứng dụng, không chỉ log gửi phím. |
   | Nhập liệu | Telex/VNI, gõ chậm/burst, dấu và undo, Backspace vật lý, Space/Enter/Tab, macro; dùng lại corpus hiện có. |
   | Vòng đời | Chọn IM và gõ ngay, login/restart fcitx5, chuyển IM lặp lại, máy đang tải, thiết bị/ACL sẵn sàng muộn; không treo event loop hoặc rò virtual keyboard/fd/worker. |
   | Đóng gói | Fresh install, upgrade từ service cũ, reinstall và remove/purge; desktop active có ACL và không cần enable service. Kiểm tra các gói và cấu hình NixOS tạo ra. |

   Chấp nhận khi không có regression mới so với baseline đã làm rõ, đường transport mới vượt các case lỗi/hủy, và thao tác cài rồi chọn bộ gõ được xác nhận bằng input thật. Nếu chưa có môi trường kiểm thử một distro hoặc desktop, ghi rõ phần chưa xác minh trước phát hành.

Thứ tự thay đổi nên là các commit nhỏ: baseline/test, injector độc lập, tích hợp engine, quyền thiết bị, migration/packaging. Chỉ đổi timing hoặc logic tiếng Việt trong một thay đổi riêng khi có lỗi tái hiện chứng minh cần thiết.
