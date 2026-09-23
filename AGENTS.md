# AGENTS.md

# QuorumESP

QuorumESP là một thiết bị quorum mạng sử dụng ESP32, được thiết kế để cung
cấp một quorum vote tương thích với Corosync QDevice cho cụm Proxmox VE.

Mục tiêu chính là xây dựng một QDevice appliance embedded thực sự có khả năng
tương tác với Corosync, không phải một dịch vụ quorum giả lập hoặc một hệ
thống quản lý cluster riêng.

---

## 1. Mục tiêu dự án

QuorumESP hướng tới việc triển khai các chức năng cần thiết để ESP32 có thể
đóng vai trò là thiết bị quorum mạng bên ngoài cho Corosync cluster.

Mục tiêu chính:

- Proxmox VE
- Corosync
- corosync-qdevice
- corosync-qnetd network quorum model
- ESP32 có Ethernet

Kiến trúc mục tiêu:

    Proxmox Node A
          |
    corosync-qdevice
          |
          | TLS / QDevice network protocol
          |
      QuorumESP
          |
          | TLS / QDevice network protocol
          |
    corosync-qdevice
          |
    Proxmox Node B


QuorumESP cung cấp quorum vote thông qua QDevice protocol.

QuorumESP KHÔNG được trở thành một cluster manager độc lập.

QuorumESP KHÔNG được trực tiếp điều khiển Proxmox node.

QuorumESP KHÔNG được thay thế logic quyết định quorum của Corosync.

---

## 2. Nguyên tắc thiết kế cốt lõi

Quy tắc kiến trúc quan trọng nhất:

    QuorumESP cung cấp một QDevice vote.
    Corosync quyết định trạng thái quorum của cluster.

Không tự triển khai một cơ chế quyết định quorum riêng nếu không có yêu cầu
rõ ràng từ thiết kế dự án.

ESP32 không được tự quyết định:

- Proxmox node nào được tiếp tục hoạt động
- Node nào trở thành primary
- Node nào phải bị cô lập
- Node nào phải bị fencing
- Cluster có được coi là healthy hay không

Những quyết định này thuộc về Corosync và cluster stack.

---

## 3. Yêu cầu an toàn

Quorum software là infrastructure có yêu cầu an toàn cao.

Bug trong QuorumESP có thể dẫn đến:

- mất quorum
- cluster hoạt động không đúng
- split-brain
- mất khả năng quản lý cluster
- hành vi không mong muốn của VM hoặc storage

Vì vậy:

1. Không được đoán hành vi của protocol.
2. Không được tự tạo protocol field.
3. Không được đoán quorum semantics.
4. Phải kiểm tra hành vi dựa trên tài liệu và source code của Corosync.
5. Khi không chắc chắn, ưu tiên fail-closed.
6. Không được giữ trạng thái quorum cũ vô thời hạn sau khi mất kết nối.
7. Không được báo trạng thái hợp lệ nếu chưa xác minh.
8. Không được bỏ qua TLS hoặc authentication trong production.
9. Không được vô hiệu hóa safety check chỉ để integration test chạy được.
10. Không được thử firmware chưa kiểm chứng trên production cluster nếu không
    có lý do rõ ràng và phương án rollback.

Nếu không chắc protocol hoạt động như thế nào, phải dừng và nghiên cứu thay vì
tự suy đoán.

---

## 4. Chiến lược phát triển

Không được bắt đầu bằng việc triển khai toàn bộ QDevice protocol trực tiếp
trên ESP32.

Project phải được phát triển từng bước.

### Phase 0 - Nghiên cứu

Phải hiểu:

- kiến trúc Corosync
- corosync-qdevice
- corosync-qnetd
- QDevice network model
- TLS
- QDevice algorithms
- membership information
- quorum voting
- client/server behavior
- connection lifecycle
- failure behavior

Ưu tiên source code và tài liệu chính thức.

Không được dùng các blog hoặc bài viết cộng đồng làm protocol specification.

---

### Phase 1 - Protocol Test Harness

Trước khi triển khai protocol trên ESP32, phải xây dựng test harness chạy trên
máy tính.

Test harness phải hỗ trợ:

- kiểm tra connection
- kiểm tra protocol message
- kiểm tra malformed message
- kiểm tra TLS
- kiểm tra mất connection
- kiểm tra reconnect
- kiểm tra nhiều client
- kiểm tra state machine

Host implementation chỉ dùng để phát triển và kiểm thử.

Không biến host implementation thành một protocol implementation thứ hai khác
với reference implementation.

---

### Phase 2 - ESP32 Network tối thiểu

Triển khai:

- Ethernet
- TCP
- TLS
- connection management
- logging
- configuration
- watchdog
- protocol state machine cơ bản

Ở phase này chưa cần UI phức tạp.

Mục tiêu là tạo network stack ổn định.

---

### Phase 3 - QDevice Protocol

Triển khai phần protocol tối thiểu cần thiết để
`corosync-qdevice` có thể giao tiếp thực sự với QuorumESP.

Mỗi thành phần protocol phải có:

- nguồn tham khảo
- tài liệu
- unit test
- integration test nếu có thể

Không triển khai behavior chưa được document trừ khi đã xác minh bằng
reference implementation.

---

### Phase 4 - Corosync Integration

Phải kiểm thử với Corosync thật.

Môi trường tối thiểu:

    Node A
    Node B
    QuorumESP

Các trường hợp phải kiểm thử:

1. Tất cả node online.
2. Ngắt Node A.
3. Ngắt Node B.
4. Ngắt QuorumESP.
5. Ngắt Node A và QuorumESP.
6. Ngắt Node B và QuorumESP.
7. Network interruption.
8. ESP32 reboot.
9. ESP32 mất nguồn.
10. Corosync restart.
11. corosync-qdevice restart.
12. TLS failure.
13. Certificate/credential không hợp lệ.
14. Reconnect sau khi lỗi.

Không được tuyên bố project tương thích trước khi hiểu rõ các behavior trên.

---

## 5. Hardware

Hardware ưu tiên là ESP32 có Ethernet.

Ethernet được ưu tiên hơn Wi-Fi vì QuorumESP là infrastructure component.

Có thể sử dụng:

- ESP32-S3 + Ethernet PHY
- ESP32 + W5500
- board ESP32 có Ethernet tích hợp

Core QDevice không được phụ thuộc cứng vào một Ethernet controller duy nhất.

Firmware phải cho phép thay đổi hardware về sau.

---

## 6. Kiến trúc firmware

Firmware phải được chia module.

Cấu trúc đề xuất:

    firmware/
    ├── main/
    │   └── main.c
    │
    ├── qdevice/
    │   ├── protocol.c
    │   ├── protocol.h
    │   ├── session.c
    │   ├── session.h
    │   ├── state.c
    │   └── state.h
    │
    ├── quorum/
    │   ├── vote.c
    │   ├── vote.h
    │   └── membership.c
    │
    ├── network/
    │   ├── ethernet.c
    │   ├── ethernet.h
    │   ├── tls.c
    │   └── tls.h
    │
    ├── config/
    │   ├── config.c
    │   └── config.h
    │
    ├── storage/
    │   ├── storage.c
    │   └── storage.h
    │
    ├── watchdog/
    │   ├── watchdog.c
    │   └── watchdog.h
    │
    ├── web/
    │   ├── api.c
    │   └── api.h
    │
    └── diagnostics/
        ├── logging.c
        └── logging.h

Có thể thay đổi cấu trúc nếu có lý do kỹ thuật rõ ràng.

Không tạo abstraction không cần thiết.

---

## 7. State machine

QDevice implementation phải sử dụng state machine rõ ràng.

Ví dụ:

    INIT
      |
      v
    NETWORK_READY
      |
      v
    TLS_CONNECTING
      |
      v
    CONNECTED
      |
      v
    PROTOCOL_HANDSHAKE
      |
      v
    ACTIVE
      |
      +------> DISCONNECTED
                    |
                    v
                RECONNECTING

State thực tế phải tuân theo protocol.

Không dùng quá nhiều boolean rời rạc để đại diện cho protocol state nếu một
state machine rõ ràng phù hợp hơn.

Transition không hợp lệ phải bị từ chối.

---

## 8. Xử lý lỗi

Behavior mặc định khi communication failure phải mang tính bảo thủ.

Ví dụ:

- TCP connection mất -> invalidate session hiện tại.
- TLS failure -> không chuyển sang ACTIVE.
- Protocol violation -> kết thúc session.
- Message không hợp lệ -> reject.
- Timeout -> không giả định peer vẫn còn hoạt động.
- Watchdog timeout -> restart ESP32.
- Configuration lỗi -> chuyển sang safe configuration mode.
- Firmware validation thất bại -> rollback nếu OTA được triển khai.

Không được tự tạo một trạng thái hợp lệ để "cứ tiếp tục chạy" sau protocol error.

---

## 9. TLS và bảo mật

Production communication phải sử dụng TLS khi QDevice network model yêu cầu.

Không được:

- tắt certificate validation vĩnh viễn
- hard-code private key vào source code
- commit production credential
- commit certificate/private key thật
- mặc định sử dụng plaintext communication trong production

Các chế độ insecure chỉ dành cho development phải:

- được bật rõ ràng
- được document rõ ràng
- không thể dễ dàng nhầm với production mode

Không commit secret vào Git.

---

## 10. Configuration

Configuration về sau nên hỗ trợ:

- hostname
- static IP
- DHCP
- gateway
- DNS
- QDevice server configuration
- cluster configuration nếu cần
- TLS certificate
- device identity
- logging level
- management interface

Configuration phải được lưu persistent qua reboot.

Configuration format phải có version.

Nếu format thay đổi, phải có migration hoặc safe reset.

---

## 11. Watchdog

QuorumESP phải có watchdog.

Tối thiểu:

- hardware watchdog
- task watchdog khi phù hợp
- network/session health monitoring

Một QDevice bị deadlock cuối cùng phải ngừng hoạt động như thể nó vẫn bình
thường.

Không được thiết kế watchdog chỉ để giữ ESP32 "sống" trong khi QDevice state
machine đã deadlock.

---

## 12. OTA

OTA không bắt buộc trong giai đoạn đầu.

Khi triển khai OTA, phải có rollback.

Mô hình ưu tiên:

    Firmware hiện tại
          |
          v
    Cài firmware mới
          |
          v
        Reboot
          |
          v
      Health check
       /       \
     PASS      FAIL
      |          |
      v          v
    Commit     Rollback

Không tự động commit firmware mới nếu chưa kiểm tra health.

---

## 13. Web Interface

Web interface chỉ nên được thêm sau khi QDevice core ổn định.

Web UI dùng để diagnostic, không phải để điều khiển quorum.

Có thể hiển thị:

- device status
- uptime
- firmware version
- network status
- Ethernet link
- TLS status
- QDevice connection
- protocol state
- cluster/node information nếu protocol cung cấp
- logs
- configuration

Không thêm chức năng cho phép người dùng tùy ý thay đổi quorum state.

---

## 14. Hardware UI

Có thể thêm:

- OLED
- LED trạng thái
- physical button
- buzzer

Ví dụ:

    QuorumESP
    Status: ONLINE
    QDevice: ACTIVE
    Cluster: proxmox-home
    Vote: 1

Physical button có thể dùng cho:

- safe mode
- reset configuration
- diagnostic mode

Không cho phép button trực tiếp bật/tắt production quorum vote nếu behavior
chưa được thiết kế và kiểm thử đầy đủ.

---

## 15. Logging

Log phải đủ để debug protocol.

Các category đề xuất:

- BOOT
- NETWORK
- TLS
- QDEVICE
- PROTOCOL
- STATE
- WATCHDOG
- CONFIG
- OTA
- WEB

Ví dụ:

    [QDEVICE] connection established
    [TLS] peer certificate verified
    [PROTOCOL] handshake started
    [STATE] CONNECTING -> ACTIVE

Không log:

- private key
- password
- authentication secret
- certificate private material

---

## 16. Testing

Testing là bắt buộc.

### Unit test

Phải test:

- state machine
- configuration parser
- protocol parser
- protocol serializer
- timeout
- malformed packet
- connection lifecycle
- quorum-related logic

### Integration test

Phải test với:

- corosync-qdevice thật
- corosync-qnetd thật khi phù hợp
- Proxmox node thật

### Failure test

Phải test:

- network loss
- packet loss
- packet delay
- connection reset
- TLS failure
- ESP32 reboot
- power loss
- corrupted configuration
- firmware rollback
- reconnect liên tục

---

## 17. Git

Mọi thay đổi có ý nghĩa phải được commit vào Git.

Không để một lượng lớn thay đổi chưa commit.

Commit nên nhỏ và có mục đích rõ ràng.

Format đề xuất:

    feat: implement qdevice session state machine
    fix: reject invalid protocol state transition
    test: add TLS reconnect tests
    docs: document qdevice protocol assumptions

Không commit:

- secret
- private key
- credential
- production certificate
- build artifact
- file debug tạm thời

---

## 18. CI

CI là bắt buộc.

AI agent không được chuyển sang implementation phase tiếp theo nếu CI đang
fail.

CI tối thiểu về sau phải kiểm tra:

1. Formatting.
2. Static analysis.
3. Unit tests.
4. Host-side protocol tests.
5. Firmware compilation.
6. Integration tests phù hợp.

Khi CI fail:

    STOP
      |
      v
    Điều tra
      |
      v
    Sửa
      |
      v
    Chạy CI lại
      |
      v
    PASS
      |
      v
    Tiếp tục

Không được bỏ qua test chỉ vì cho rằng lỗi "không liên quan".

Nếu test thực sự lỗi thời, phải sửa test và document lý do.

---

## 19. Quy trình phát triển

Với mỗi task:

1. Kiểm tra implementation hiện tại.
2. Xác định architecture liên quan.
3. Nghiên cứu protocol nếu có phần chưa biết.
4. Implement thay đổi nhỏ nhất có thể.
5. Thêm hoặc cập nhật test.
6. Chạy formatter/static analysis.
7. Chạy test.
8. Chạy CI.
9. Review diff.
10. Commit.
11. Chỉ sau đó mới tiếp tục.

Không rewrite toàn bộ project nếu không có lý do cụ thể.

Không thay framework đang hoạt động chỉ vì agent thích framework khác.

---

## 20. Quy tắc nghiên cứu protocol

Khi triển khai Corosync/QDevice:

Ưu tiên nguồn theo thứ tự:

1. Corosync source code chính thức.
2. Corosync documentation chính thức.
3. Proxmox documentation/source code.
4. Behavior của reference implementation.
5. Technical documentation độc lập.
6. Community discussion.

Community discussion có thể dùng để phát hiện vấn đề nhưng không được coi là
protocol specification.

Mọi quyết định protocol không hiển nhiên phải được document.

Ví dụ:

    docs/protocol.md

    ## Session handshake

    Nguồn:
    <reference>

    Behavior quan sát được:
    <test result>

    Implementation:
    <description>

---

## 21. Không nhầm QDevice và QNetD

Project phải phân biệt rõ:

    corosync-qdevice
    corosync-qnetd

Thông thường Proxmox node chạy QDevice client.

Network quorum service bên ngoài thường do qnetd cung cấp.

QuorumESP hướng tới việc triển khai phần network-side cần thiết để giao tiếp
với corosync-qdevice.

Không được dùng "QDevice" và "QNetD" lẫn nhau trong source code hoặc tài liệu.

---

## 22. Phạm vi project

Phạm vi ban đầu:

- ESP32
- Ethernet
- Corosync QDevice network protocol
- TLS
- quorum vote
- reliability
- diagnostics
- Proxmox integration
- testing

Không nằm trong phạm vi ban đầu:

- cluster management
- VM management
- storage management
- automatic fencing
- SSH management Proxmox
- custom cluster election
- thay thế Corosync
- thay thế Proxmox
- hosted service
- cloud dependency không cần thiết

Không mở rộng scope nếu chưa có lý do được document.

---

## 23. Production Safety

QuorumESP được coi là experimental cho tới khi interoperability và failure
behavior được kiểm thử đầy đủ.

Agent phải phân biệt rõ:

    Experimental
    Development
    Integration Tested
    Production Candidate

Không được gọi firmware là production-ready chỉ vì firmware compile thành công.

Production readiness phải dựa trên integration test và failure test.

---

## 24. Definition of Done

Một feature không được coi là hoàn thành chỉ vì code compile được.

Feature chỉ hoàn thành khi:

- implementation tồn tại
- architecture được document
- test phù hợp đã tồn tại
- failure behavior đã được xác định
- CI pass
- không có known critical regression
- Git diff đã được review
- thay đổi đã được commit

Đối với protocol functionality, phải chứng minh interoperability với reference
implementation khi có thể.

---

## 25. Thứ tự ưu tiên hiện tại

Ưu tiên KHÔNG phải web interface.

Thứ tự:

    1. Hiểu QDevice/QNetD architecture.
    2. Xác định chính xác protocol boundary.
    3. Xây dựng host-side protocol test harness.
    4. Chứng minh interoperability với corosync-qdevice.
    5. Implement protocol tối thiểu trên ESP32.
    6. Ethernet reliability.
    7. TLS/security.
    8. Watchdog và failure recovery.
    9. Test với Proxmox cluster thật.
    10. Management UI và appliance features.

Không bắt đầu bằng dashboard.

Protocol và reliability mới là phần cốt lõi của project.

---

## 26. Quy tắc cho AI Agent

AI agent phải:

- kiểm tra các assumption sai
- chỉ ra protocol chưa chắc chắn
- ưu tiên bằng chứng thay vì suy đoán
- giữ nguyên behavior đang hoạt động
- thay đổi từng bước nhỏ
- test trước khi tiếp tục
- dừng khi CI fail
- document các quyết định quan trọng
- ưu tiên correctness hơn tốc độ

AI agent không được:

- tự bịa protocol behavior
- tuyên bố interoperability khi chưa test
- âm thầm tắt security
- âm thầm làm yếu test
- bypass CI
- thay đổi quorum semantics mà không có lý do rõ ràng
- thêm dependency không cần thiết
- biến QuorumESP thành cluster manager riêng

Nếu yêu cầu mới xung đột với tính an toàn của cluster, phải dừng và giải thích
xung đột trước khi implementation.