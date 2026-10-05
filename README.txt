IOT LAB 3
==========================================

Luồng:
DHT11 -> ESP8266
       -> ThingSpeak
       -> Supabase
       -> Node.js Backend -> HTML Dashboard

Latency:
t0,t1,t2,t3: ESP8266
t4,t5,t6: Node.js backend
t7: browser nhận SSE

Công thức:
L_e2e    = t7 - t0
L_uplink = t4 - t2
L_cloud  = t5 - t4
L_notify = t7 - t6
L_device = t1 - t0

Latency chỉ tạo khi temperature >= 30 C.
Supabase chỉ lưu sensor history, không lưu latency.

Chạy backend:
cd backend
node server.js

Chạy frontend:
Mở frontend/index.html bằng trình duyệt.
Hoặc dùng VS Code Live Server.

Nếu frontend không kết nối backend localhost:3000:
Mở Console:
localStorage.setItem("IOT_BACKEND_URL", "http://<IP-PC>:3000");
location.reload();

Lưu ý:
ESP8266 sketch chứa placeholder credential. Copy credential từ file lab1_supabase(1).ino của bạn vào máy local.
