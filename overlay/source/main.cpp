#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "sirene_ipc.h"

namespace {

static std::string fmtTimeStr(double sec) {
    if (sec < 0.0) sec = 0.0;
    int s = static_cast<int>(sec + 0.5);
    int m = s / 60;
    int r = s % 60;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, r);
    return std::string(buf);
}

static const char* modeLabel(uint32_t m) {
    switch (m) {
        case SIR_MODE_LOOP_ONE: return "Loop One";
        case SIR_MODE_SHUFFLE:  return "Shuffle";
        default:                return "Loop All";
    }
}

static const char* stateLabel(uint32_t s) {
    switch (s) {
        case SIR_STATE_PLAYING: return "Playing";
        case SIR_STATE_PAUSED:  return "Paused";
        case SIR_STATE_OPENING: return "Loading...";
        case SIR_STATE_ERROR:   return "Decode Error";
        default:                return "Idle";
    }
}

class SireneQueueGui : public tsl::Gui {
public:
    explicit SireneQueueGui(uint32_t count) : m_count(count) {}

    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Sirène Queue", "Select track to play");
        auto* list  = new tsl::elm::List();

        uint32_t limit = m_count > 120 ? 120 : m_count;
        if (limit == 0) {
            list->addItem(new tsl::elm::ListItem("Queue is empty (Scan sdmc:/music)"));
        } else {
            for (uint32_t i = 0; i < limit; i++) {
                SireneBgQueueItem item;
                if (sirene_ipc_get_queue_item(i, &item) != 0) continue;
                std::string label = item.title[0] ? item.title : item.path;
                if (label.size() > 34) label = label.substr(0, 31) + "...";
                auto* row = new tsl::elm::ListItem(label);
                row->setClickListener([i](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        sirene_ipc_queue_play_index(static_cast<int32_t>(i), 0.0);
                        tsl::goBack();
                        return true;
                    }
                    return false;
                });
                list->addItem(row);
            }
        }

        frame->setContent(list);
        return frame;
    }

private:
    uint32_t m_count;
};

class SireneMainGui : public tsl::Gui {
public:
    SireneMainGui() {
        std::memset(&m_status, 0, sizeof(m_status));
    }

    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Sirène", "v1.2.10 · Background");
        auto* list  = new tsl::elm::List();

        m_connected = (sirene_ipc_connect() == 0);
        if (m_connected) {
            m_connected = (sirene_ipc_get_status(&m_status) == 0);
        }

        if (!m_connected) {
            list->addItem(new tsl::elm::ListItem("sys-sirene is not running"));
            list->addItem(new tsl::elm::ListItem("Enable TID 4200000000051234"));
            frame->setContent(list);
            return frame;
        }

        /* 顶部正在播放状态与进度条卡片 */
        list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
            const char* title = m_status.title[0] ? m_status.title : "No Track Playing";
            std::string tstr(title);
            if (tstr.size() > 30) tstr = tstr.substr(0, 27) + "...";
            renderer->drawString(tstr.c_str(), false, x + 8, y + 26, 20, renderer->a(0xFFFF));

            std::string sub = std::string(stateLabel(m_status.state)) + "  " +
                              fmtTimeStr(m_status.position_sec) + " / " + fmtTimeStr(m_status.duration_sec);
            renderer->drawString(sub.c_str(), false, x + 8, y + 52, 16, renderer->a(0xAAAA));

            s32 barX = x + 8;
            s32 barY = y + 66;
            s32 barW = w - 16;
            s32 barH = 6;
            renderer->drawRect(barX, barY, barW, barH, renderer->a(0x5555));
            double frac = (m_status.duration_sec > 0.1) ? (m_status.position_sec / m_status.duration_sec) : 0.0;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            s32 fillW = static_cast<s32>(barW * frac);
            if (fillW > 0) {
                renderer->drawRect(barX, barY, fillW, barH, renderer->a(0xF6A2));
            }
        }), 82);

        m_itemPlay = new tsl::elm::ListItem("Play / Pause");
        m_itemPlay->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                sirene_ipc_set_paused(2);
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(m_itemPlay);

        auto* itemNext = new tsl::elm::ListItem("Next Track");
        itemNext->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                sirene_ipc_next();
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(itemNext);

        auto* itemPrev = new tsl::elm::ListItem("Previous Track");
        itemPrev->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                sirene_ipc_prev();
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(itemPrev);

        m_itemVol = new tsl::elm::ListItem("Volume  (L/R or A +5%)");
        m_itemVol->setClickListener([this](u64 keys) {
            if (keys & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_R)) {
                adjustVolume(+0.05f);
                return true;
            }
            if (keys & (HidNpadButton_Left | HidNpadButton_L)) {
                adjustVolume(-0.05f);
                return true;
            }
            return false;
        });
        list->addItem(m_itemVol);

        m_itemMode = new tsl::elm::ListItem("Loop Mode");
        m_itemMode->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                uint32_t nxt = (m_status.play_mode + 1u) % SIR_MODE_COUNT;
                sirene_ipc_set_mode(nxt);
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(m_itemMode);

        m_itemQueue = new tsl::elm::ListItem("Track List");
        m_itemQueue->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<SireneQueueGui>(m_status.queue_count);
                return true;
            }
            return false;
        });
        list->addItem(m_itemQueue);

        auto* itemScan = new tsl::elm::ListItem("Scan sdmc:/music");
        itemScan->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                uint32_t cnt = 0;
                sirene_ipc_scan_library(&cnt);
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(itemScan);

        auto* itemStop = new tsl::elm::ListItem("Stop Background Audio");
        itemStop->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                sirene_ipc_stop();
                refreshStatus();
                return true;
            }
            return false;
        });
        list->addItem(itemStop);

        updateLabels();
        frame->setContent(list);
        return frame;
    }

    virtual void update() override {
        if (!m_connected) return;
        if (++m_tick >= 15) {
            m_tick = 0;
            refreshStatus();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
        (void)keysHeld;
        (void)touchPos;
        (void)joyStickPosLeft;
        (void)joyStickPosRight;
        if (!m_connected) return false;
        if (keysDown & HidNpadButton_ZR) {
            adjustVolume(+0.05f);
            return true;
        }
        if (keysDown & HidNpadButton_ZL) {
            adjustVolume(-0.05f);
            return true;
        }
        return false;
    }

private:
    bool m_connected = false;
    int  m_tick      = 0;
    SireneBgStatus m_status;
    tsl::elm::ListItem* m_itemPlay  = nullptr;
    tsl::elm::ListItem* m_itemVol   = nullptr;
    tsl::elm::ListItem* m_itemMode  = nullptr;
    tsl::elm::ListItem* m_itemQueue = nullptr;

    void adjustVolume(float delta) {
        float v = m_status.volume + delta;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        sirene_ipc_set_volume(v);
        refreshStatus();
    }

    void refreshStatus() {
        if (sirene_ipc_get_status(&m_status) == 0) {
            updateLabels();
        }
    }

    void updateLabels() {
        if (m_itemPlay) {
            m_itemPlay->setValue(m_status.state == SIR_STATE_PLAYING ? "Playing" : "Paused");
        }
        if (m_itemVol) {
            char vbuf[16];
            std::snprintf(vbuf, sizeof(vbuf), "%d%%", static_cast<int>(m_status.volume * 100.0f + 0.5f));
            m_itemVol->setValue(vbuf);
        }
        if (m_itemMode) {
            m_itemMode->setValue(modeLabel(m_status.play_mode));
        }
        if (m_itemQueue) {
            char qbuf[24];
            std::snprintf(qbuf, sizeof(qbuf), "%u", static_cast<unsigned>(m_status.queue_count));
            m_itemQueue->setValue(qbuf);
        }
    }
};

class SireneOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        sirene_ipc_connect();
    }

    virtual void exitServices() override {
        sirene_ipc_disconnect();
    }

    virtual void onShow() override {
        sirene_ipc_connect();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<SireneMainGui>();
    }
};

} /* namespace */

int main(int argc, char** argv) {
    return tsl::loop<SireneOverlay>(argc, argv);
}
