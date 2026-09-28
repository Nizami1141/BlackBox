"""GUI: команда съёмки ESP32-S3, сохранение JPEG и подтверждение LCD."""

from __future__ import annotations

import datetime as dt
from pathlib import Path
import queue
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
from serial.tools import list_ports


BAUD = 115200


class CameraGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("ESP32-S3 — камера и LCD")
        self.root.geometry("720x520")
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.busy = False

        top = ttk.Frame(root, padding=12)
        top.pack(fill="x")
        ttk.Label(top, text="COM-порт:").pack(side="left")
        self.port = tk.StringVar(value="COM9")
        self.port_box = ttk.Combobox(top, textvariable=self.port, width=12)
        self.port_box.pack(side="left", padx=(6, 14))
        ttk.Button(top, text="Обновить порты", command=self.refresh_ports).pack(side="left")

        directory_row = ttk.Frame(root, padding=(12, 0, 12, 8))
        directory_row.pack(fill="x")
        ttk.Label(directory_row, text="Сохранять в:").pack(side="left")
        self.output = tk.StringVar(value=str((Path.cwd() / "camera_test_photos").resolve()))
        ttk.Entry(directory_row, textvariable=self.output).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(directory_row, text="Выбрать", command=self.choose_directory).pack(side="left")

        self.capture_button = ttk.Button(root, text="СФОТОГРАФИРОВАТЬ", command=self.capture)
        self.capture_button.pack(fill="x", padx=12, pady=8, ipady=12)

        self.status = tk.StringVar(value="Готово. Прошейте s3_camera_lcd_test и нажмите кнопку.")
        ttk.Label(root, textvariable=self.status, padding=(12, 4)).pack(fill="x")

        self.log = tk.Text(root, height=20, wrap="word", state="disabled")
        self.log.pack(fill="both", expand=True, padx=12, pady=(4, 12))
        self.refresh_ports()
        self.root.after(100, self.process_events)

    def refresh_ports(self) -> None:
        ports = [p.device for p in list_ports.comports()]
        self.port_box["values"] = ports
        if self.port.get() not in ports and ports:
            preferred = next((p for p in ports if p.upper() == "COM9"), ports[0])
            self.port.set(preferred)

    def choose_directory(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.output.get())
        if selected:
            self.output.set(selected)

    def append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def capture(self) -> None:
        if self.busy:
            return
        self.busy = True
        self.capture_button.configure(state="disabled")
        self.status.set("Подключение и съёмка...")
        threading.Thread(target=self.capture_worker, daemon=True).start()

    @staticmethod
    def read_exact(port: serial.Serial, length: int) -> bytes:
        data = bytearray()
        deadline = time.monotonic() + 20
        while len(data) < length and time.monotonic() < deadline:
            chunk = port.read(length - len(data))
            if chunk:
                data.extend(chunk)
        return bytes(data)

    def capture_worker(self) -> None:
        try:
            target_dir = Path(self.output.get())
            target_dir.mkdir(parents=True, exist_ok=True)
            self.events.put(("log", f"Открываю {self.port.get()} @ {BAUD}..."))
            with serial.Serial(self.port.get(), BAUD, timeout=1, write_timeout=3) as port:
                port.dtr = False
                port.rts = False
                time.sleep(0.4)
                port.reset_input_buffer()
                port.write(b"STATUS\n")
                port.flush()

                ready_deadline = time.monotonic() + 15
                ready = False
                while time.monotonic() < ready_deadline:
                    line = port.readline().strip()
                    if not line:
                        continue
                    decoded = line.decode(errors="replace")
                    self.events.put(("log", "ESP: " + decoded))
                    if line.startswith(b"READY "):
                        ready = b"CAMERA=OK" in line and b"LCD=OK" in line
                        break
                if not ready:
                    raise RuntimeError("Плата не сообщила CAMERA=OK и LCD=OK")

                port.write(b"CAPTURE\n")
                port.flush()
                deadline = time.monotonic() + 20
                header = b""
                while time.monotonic() < deadline:
                    line = port.readline().strip()
                    if not line:
                        continue
                    if line.startswith(b"JPEG_BEGIN "):
                        header = line
                        break
                    decoded = line.decode(errors="replace")
                    self.events.put(("log", "ESP: " + decoded))
                    if line.startswith(b"CAPTURE_FAIL"):
                        raise RuntimeError(decoded)
                if not header:
                    raise TimeoutError("ESP32 не передала заголовок JPEG")

                fields = header.split()
                if len(fields) != 4:
                    raise RuntimeError(f"Некорректный JPEG-заголовок: {header!r}")
                length, width, height = map(int, fields[1:])
                image = self.read_exact(port, length)
                if len(image) != length:
                    raise RuntimeError(f"Получено {len(image)} из {length} байт")
                if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
                    raise RuntimeError("Полученные данные не являются целым JPEG")

                stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
                destination = target_dir / f"ov5640_{stamp}_{width}x{height}.jpg"
                destination.write_bytes(image)
                port.write(b"SAVED\n")
                port.flush()

            self.events.put(("log", f"PASS: JPEG {width}x{height}, {length} байт"))
            self.events.put(("success", destination))
        except Exception as exc:
            self.events.put(("error", f"{type(exc).__name__}: {exc}"))

    def process_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.append_log(str(value))
                elif kind == "success":
                    self.busy = False
                    self.capture_button.configure(state="normal")
                    self.status.set(f"Сохранено: {value}")
                    self.append_log(f"Файл сохранён: {value}")
                    messagebox.showinfo("Готово", f"Фотография сохранена:\n{value}\n\nНа LCD должно быть OK.")
                elif kind == "error":
                    self.busy = False
                    self.capture_button.configure(state="normal")
                    self.status.set("Ошибка — смотрите журнал")
                    self.append_log("FAIL: " + str(value))
                    messagebox.showerror("Ошибка", str(value))
        except queue.Empty:
            pass
        self.root.after(100, self.process_events)


def main() -> None:
    root = tk.Tk()
    CameraGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
