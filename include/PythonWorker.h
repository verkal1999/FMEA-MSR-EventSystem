#pragma once
#include <pybind11/embed.h>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <future>
#include <optional>
#include <type_traits>
#include <memory>

namespace py = pybind11;

class PythonWorker {
public:
    static PythonWorker& instance() {
        static PythonWorker w;
        return w;
    }

    // Startet den Worker-Thread (einmalig)
    void start() {
        std::lock_guard<std::mutex> lk(mx_);
        if (running_) return;
        running_ = true;
        th_ = std::thread([this]{ this->run(); });
    }

    // Stop + join (vor Programmende aufrufen)
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            running_ = false;
        }
        cv_.notify_one();
        if (th_.joinable()) th_.join();
    }

    // Führe einen Job im Python-Thread aus und liefere Ergebnis zurück
    template <class F>
    auto call(F&& f) -> decltype(f()) {
        using R = std::invoke_result_t<F&>;

        // 1) callable F in shared_ptr legen (auch wenn F move-only ist)
        auto fn = std::make_shared<std::decay_t<F>>(std::forward<F>(f));

        // 2) Rückgabeweg über promise/future, ebenfalls in shared_ptr
        auto prom = std::make_shared<std::promise<R>>();
        auto fut  = prom->get_future();

        {
            std::lock_guard<std::mutex> lk(mx_);
            // 3) Das Job-Lambda enthält NUR shared_ptr-Kopien -> copy-constructible
            q_.emplace([fn = std::move(fn), prom = std::move(prom)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        (*fn)();                      // F ausführen
                        prom->set_value();            // void-Fall signalisieren
                    } else {
                        prom->set_value((*fn)());     // Wert setzen
                    }
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
            });
        }
        cv_.notify_one();

        if constexpr (std::is_void_v<R>) {
            fut.get();        // wartet nur auf Abschluss
        } else {
            return fut.get(); // Wert/Exception propagieren
        }
    }

private:
    void run() {
        // Einziger Ort im Prozess, der den GIL länger hält.
        py::gil_scoped_acquire gil;

        // (Debug) einmal sys.path loggen – optional
        try {
            py::module_ sys = py::module_::import("sys");
            py::list path = sys.attr("path").cast<py::list>();
            // Falls dein src-Ordner noch nicht drin ist:
            // path.append(R"(C:\Users\...\Test\src)");
        } catch (...) {}

        for(;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [&]{ return !q_.empty() || !running_; });
                if (!running_ && q_.empty()) break;
                job = std::move(q_.front());
                q_.pop();
            }
            // Job im Python-Thread ausführen
            try {
                job();
            } catch (...) {
                // Exceptions propagieren sich über packaged_task/future zum Aufrufer
            }
        }
        // gil_scoped_acquire fällt hier aus dem Scope → GIL frei
    }

    std::thread th_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> q_;
    bool running_ = false;
};
