<script setup>
import { reactive, computed, onMounted, onBeforeUnmount, toRef } from 'vue'
import { useI18n } from 'vue-i18n'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'

const { t } = useI18n()

const props = defineProps({
  platform: {
    type: String,
    required: true,
  },
  config: {
    type: Object,
    required: true,
  },
})

const config = toRef(props, 'config')

/**
 * 创建一个驱动管理器，统一封装状态/加载/操作逻辑，避免 vmouse / vigem 的重复代码。
 * @param {string} driverKey - window 上挂载的驱动对象属性名（如 'vmouseDriver'）
 * @param {object} initialStatus - 初始状态对象
 */
function createDriverManager(driverKey, initialStatus) {
  let refreshTimeoutId = null

  const driver = () => window[driverKey]

  function dispose() {
    if (refreshTimeoutId !== null) {
      clearTimeout(refreshTimeoutId)
      refreshTimeoutId = null
    }
  }

  const manager = reactive({
    available: false,
    status: { ...initialStatus },
    loading: false,
    operating: false,

    get dotClass() {
      if (manager.status.running) return 'dot-active'
      if (manager.status.installed) return 'dot-warning'
      return 'dot-inactive'
    },

    async refresh() {
      manager.loading = true
      try {
        const api = driver()
        if (!api) return
        manager.status = await api.getStatus()
      } catch {
        /* ignore */
      } finally {
        manager.loading = false
      }
    },

    async runOp(confirmKey, op) {
      if (!confirm(t(confirmKey))) return

      const api = driver()
      if (!api) return

      manager.operating = true
      try {
        await op(api)
        dispose()
        refreshTimeoutId = window.setTimeout(() => {
          refreshTimeoutId = null
          manager.refresh()
        }, 2000)
      } catch (e) {
        alert(e instanceof Error ? e.message : String(e))
      } finally {
        manager.operating = false
      }
    },

    dispose,
  })

  return manager
}

// vmouse 驱动管理
const vmouse = createDriverManager('vmouseDriver', { installed: false, running: false, status_text: '' })
const installVmouse = () => vmouse.runOp('config.vmouse_confirm_install', d => d.install())
const uninstallVmouse = () => vmouse.runOp('config.vmouse_confirm_uninstall', d => d.uninstall())

const vmouseStatusLabel = computed(() => {
  if (vmouse.loading) return t('config.vmouse_status_checking')
  if (vmouse.status.running) return t('config.vmouse_status_running')
  if (vmouse.status.installed) return t('config.vmouse_status_installed')
  return t('config.vmouse_status_not_installed')
})

// ViGEmBus 虚拟手柄驱动管理
const vigem = createDriverManager('vigemDriver',
  { installed: false, running: false, version: '', version_ok: false, status_text: '' })
const installVigem = (force = false) => vigem.runOp(
  force ? 'config.vigem_confirm_reinstall' : 'config.vigem_confirm_install',
  d => d.install(force)
)
const uninstallVigem = () => vigem.runOp('config.vigem_confirm_uninstall', d => d.uninstall())

const vigemStatusLabel = computed(() => {
  if (vigem.loading) return t('config.vigem_status_checking')

  const s = vigem.status
  if (!s.installed) return t('config.vigem_status_not_installed')
  if (!s.version_ok) return t('config.vigem_status_outdated')
  if (s.running) {
    const running = t('config.vigem_status_running')
    return s.version ? `${running} (${s.version})` : running
  }
  return t('config.vigem_status_installed')
})

const vigemDotClass = computed(() => {
  const s = vigem.status
  if (s.installed && s.version_ok) return 'dot-active'
  if (s.installed) return 'dot-warning'
  return 'dot-inactive'
})

onMounted(async () => {
  if (!window.isTauri) return
  vmouse.available = !!window.vmouseDriver
  vigem.available = !!window.vigemDriver
  await Promise.all([
    vmouse.available ? vmouse.refresh() : Promise.resolve(),
    vigem.available ? vigem.refresh() : Promise.resolve(),
  ])
})

onBeforeUnmount(() => {
  vmouse.dispose()
  vigem.dispose()
})
</script>

<template>
  <div id="input" class="config-page">
    <div class="mb-3" v-if="platform === 'windows'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="usb_forwarding_enabled"
               v-model="config.usb_forwarding_enabled" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="usb_forwarding_enabled">{{ $t('config.usb_forwarding_enabled') }}</label>
      </div>
      <div class="form-text">{{ $t('config.usb_forwarding_description') }}</div>
      <div v-if="config.usb_forwarding_enabled === 'enabled'" class="mt-2">
        <label for="usb_forwarding_port" class="form-label">{{ $t('config.usb_forwarding_port') }}</label>
        <input id="usb_forwarding_port" type="number" :min="Number(config.usb_forwarding_port) === 0 ? 0 : 1024" max="65535"
               class="form-control" v-model.number="config.usb_forwarding_port">
        <div class="form-text">{{ $t('config.usb_forwarding_port_hint') }}</div>
      </div>
    </div>
    <!-- Enable Gamepad Input -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="controller" 
               v-model="config.controller" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="controller">
          {{ $t('config.controller') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.controller_desc') }}</div>
    </div>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>
          
          <template #windows>
            <option value="ds5">{{ $t('config.gamepad_ds5') }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
      <div
        v-if="platform === 'windows' && config.gamepad === 'ds5'"
        class="alert alert-warning mt-2 mb-0"
        role="note"
      >
        <i class="fas fa-info-circle me-2" aria-hidden="true"></i>
        {{ $t('config.gamepad_ds5_component_hint_windows') }}
      </div>
    </div>

    <div class="accordion mb-3" v-if="config.gamepad === 'ds4'">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.gamepad_ds4_manual') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <div>
              <label for="ds4_back_as_touchpad_click" class="form-label">{{ $t('config.ds4_back_as_touchpad_click') }}</label>
              <select id="ds4_back_as_touchpad_click" class="form-select"
                      v-model="config.ds4_back_as_touchpad_click">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.ds4_back_as_touchpad_click_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
    <div class="accordion mb-3" v-if="config.controller === 'enabled' && config.gamepad === 'auto' && platform === 'windows'">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.gamepad_auto') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="motion_as_ds4" 
                       v-model="config.motion_as_ds4" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="motion_as_ds4">
                  {{ $t('config.motion_as_ds4') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.motion_as_ds4_desc') }}</div>
            </div>
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="touchpad_as_ds4" 
                       v-model="config.touchpad_as_ds4" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="touchpad_as_ds4">
                  {{ $t('config.touchpad_as_ds4') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.touchpad_as_ds4_desc') }}</div>
            </div>
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="enable_dsu_server" 
                       v-model="config.enable_dsu_server" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="enable_dsu_server">
                  {{ $t('config.enable_dsu_server') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.enable_dsu_server_desc') }}</div>
            </div>
            <div class="mb-3" v-if="config.enable_dsu_server === 'enabled'">
              <label for="dsu_server_port" class="form-label">{{ $t('config.dsu_server_port') }}</label>
              <input type="number" class="form-control" id="dsu_server_port" placeholder="26760"
                     v-model="config.dsu_server_port" min="1024" max="65535" />
              <div class="form-text">{{ $t('config.dsu_server_port_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="keyboard" 
               v-model="config.keyboard" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="keyboard">
          {{ $t('config.keyboard') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.keyboard_desc') }}</div>
    </div>

    <!-- Key Repeat Delay-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <!-- Always send scancodes -->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="always_send_scancodes" 
               v-model="config.always_send_scancodes" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="always_send_scancodes">
          {{ $t('config.always_send_scancodes') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.always_send_scancodes_desc') }}</div>
    </div>

    <!-- Mapping Key AltRight to Key Windows -->
    <div class="mb-3" v-if="config.keyboard === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="key_rightalt_to_key_win" 
               v-model="config.key_rightalt_to_key_win" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="key_rightalt_to_key_win">
          {{ $t('config.key_rightalt_to_key_win') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.key_rightalt_to_key_win_desc') }}</div>
    </div>

    <!-- Enable Mouse Input -->
    <hr>
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="mouse" 
               v-model="config.mouse" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="mouse">
          {{ $t('config.mouse') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.mouse_desc') }}</div>
    </div>

    <!-- High resolution scrolling support -->
    <div class="mb-3" v-if="config.mouse === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="high_resolution_scrolling" 
               v-model="config.high_resolution_scrolling" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="high_resolution_scrolling">
          {{ $t('config.high_resolution_scrolling') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.high_resolution_scrolling_desc') }}</div>
    </div>

    <!-- Native pen/touch support -->
    <div class="mb-3" v-if="config.mouse === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="native_pen_touch" 
               v-model="config.native_pen_touch" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="native_pen_touch">
          {{ $t('config.native_pen_touch') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.native_pen_touch_desc') }}</div>
    </div>

    <!-- Native touchpad optimization -->
    <div class="mb-3" v-if="config.mouse === 'enabled' && platform === 'windows'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="native_touchpad_optimization"
               v-model="config.native_touchpad_optimization" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="native_touchpad_optimization">
          {{ $t('config.native_touchpad_optimization') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.native_touchpad_optimization_desc') }}</div>
    </div>

    <!-- Capture mouse cursor into the stream -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="capture_cursor"
               v-model="config.capture_cursor">
        <label class="form-check-label" for="capture_cursor">
          {{ $t('config.capture_cursor') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.capture_cursor_desc') }}</div>
    </div>

    <!-- Draw mouse cursor in AMF -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="amf_draw_mouse_cursor" 
               v-model="config.amf_draw_mouse_cursor">
        <label class="form-check-label" for="amf_draw_mouse_cursor">
          {{ $t('config.amf_draw_mouse_cursor') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.amf_draw_mouse_cursor_desc') }}</div>
    </div>

    <div
      class="mb-3 accordion"
      v-if="platform === 'windows' && ((config.controller === 'enabled' && vigem.available) || config.mouse === 'enabled')"
    >
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button
            class="accordion-button collapsed"
            type="button"
            data-bs-toggle="collapse"
            data-bs-target="#input-driver-management-collapse"
          >
            {{ $t('config.input_driver_management') }}
          </button>
        </h2>
        <div id="input-driver-management-collapse" class="accordion-collapse collapse">
          <div class="accordion-body">
            <!-- ViGEmBus virtual gamepad driver management (Windows + Tauri only) -->
            <div class="mb-3" v-if="config.controller === 'enabled' && vigem.available">
              <label class="form-label">
                {{ $t('config.vigem_label') }}
                <span class="driver-badge driver-badge-info ms-1">ViGEmBus</span>
              </label>
              <div class="form-text mb-2">{{ $t('config.vigem_desc') }}</div>
              <div class="vmouse-panel">
                <div class="vmouse-panel-header">
                  <div class="vmouse-status-indicator">
                    <span class="vmouse-dot" :class="vigemDotClass"></span>
                    <span class="vmouse-status-label">{{ vigemStatusLabel }}</span>
                  </div>
                  <button class="vmouse-refresh-btn" @click="vigem.refresh"
                          :disabled="vigem.loading" :title="$t('config.vigem_refresh')">
                    <i class="fas fa-sync-alt" :class="{ 'fa-spin': vigem.loading }"></i>
                  </button>
                </div>
                <div class="vmouse-panel-body">
                  <button v-if="!vigem.status.installed || !vigem.status.version_ok"
                          class="vmouse-action-btn vmouse-install-btn"
                          @click="installVigem(false)" :disabled="vigem.loading || vigem.operating">
                    <span v-if="vigem.operating" class="vmouse-spinner"></span>
                    <i v-else class="fas fa-download"></i>
                    <span>{{ vigem.operating
                      ? $t('config.vigem_installing')
                      : (vigem.status.installed ? $t('config.vigem_update') : $t('config.vigem_install')) }}</span>
                  </button>
                  <template v-else>
                    <button class="vmouse-action-btn vmouse-install-btn"
                            @click="installVigem(true)" :disabled="vigem.loading || vigem.operating"
                            :title="$t('config.vigem_reinstall_desc')">
                      <span v-if="vigem.operating" class="vmouse-spinner"></span>
                      <i v-else class="fas fa-sync"></i>
                      <span>{{ vigem.operating ? $t('config.vigem_installing') : $t('config.vigem_reinstall') }}</span>
                    </button>
                    <button class="vmouse-action-btn vmouse-uninstall-btn"
                            @click="uninstallVigem" :disabled="vigem.loading || vigem.operating">
                      <span v-if="vigem.operating" class="vmouse-spinner"></span>
                      <i v-else class="fas fa-trash-alt"></i>
                      <span>{{ vigem.operating ? $t('config.vigem_uninstalling') : $t('config.vigem_uninstall') }}</span>
                    </button>
                  </template>
                </div>
              </div>
            </div>

            <hr v-if="config.controller === 'enabled' && vigem.available && config.mouse === 'enabled'">

            <!-- Virtual mouse driver -->
            <div v-if="config.mouse === 'enabled'">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="virtual_mouse"
                       v-model="config.virtual_mouse" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="virtual_mouse">
                  {{ $t('config.virtual_mouse') }}
                  <span class="driver-badge driver-badge-warning ms-1">{{ $t('config.experimental') }}</span>
                </label>
              </div>
              <div class="form-text">{{ $t('config.virtual_mouse_desc') }}</div>

              <!-- Tauri 环境：驱动管理面板 -->
              <div v-if="vmouse.available" class="vmouse-panel mt-2">
                <div class="vmouse-panel-header">
                  <div class="vmouse-status-indicator">
                    <span class="vmouse-dot" :class="vmouse.dotClass"></span>
                    <span class="vmouse-status-label">{{ vmouseStatusLabel }}</span>
                  </div>
                  <button class="vmouse-refresh-btn" @click="vmouse.refresh"
                          :disabled="vmouse.loading" :title="$t('config.vmouse_refresh')">
                    <i class="fas fa-sync-alt" :class="{ 'fa-spin': vmouse.loading }"></i>
                  </button>
                </div>
                <div class="vmouse-panel-body">
                  <button v-if="!vmouse.status.installed" class="vmouse-action-btn vmouse-install-btn"
                          @click="installVmouse" :disabled="vmouse.loading || vmouse.operating">
                    <span v-if="vmouse.operating" class="vmouse-spinner"></span>
                    <i v-else class="fas fa-download"></i>
                    <span>{{ vmouse.operating ? $t('config.vmouse_installing') : $t('config.vmouse_install') }}</span>
                  </button>
                  <button v-else class="vmouse-action-btn vmouse-uninstall-btn"
                          @click="uninstallVmouse" :disabled="vmouse.loading || vmouse.operating">
                    <span v-if="vmouse.operating" class="vmouse-spinner"></span>
                    <i v-else class="fas fa-trash-alt"></i>
                    <span>{{ vmouse.operating ? $t('config.vmouse_uninstalling') : $t('config.vmouse_uninstall') }}</span>
                  </button>
                </div>
              </div>

              <!-- 非 Tauri 环境：显示提示信息 -->
              <div v-else class="vmouse-helper mt-2">
                <i class="fas fa-info-circle me-1"></i>
                <span>{{ $t('config.vmouse_note') }}</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

  </div>
</template>

<style scoped>
/* 非 Tauri 环境的提示信息 */
.vmouse-helper {
  display: flex;
  align-items: center;
  padding: 0.5rem 0.75rem;
  background: var(--ui-accent-soft);
  border-radius: var(--ui-radius-sm);
  border: 1px solid var(--ui-border);
  color: var(--ui-text-secondary);
  font-size: 0.85rem;
}

.vmouse-helper i {
  color: var(--ui-accent);
}

.driver-badge {
  display: inline-flex;
  align-items: center;
  padding: 0.2rem 0.45rem;
  border: 1px solid transparent;
  border-radius: 999px;
  font-size: 0.7em;
  font-weight: 600;
  line-height: 1;
  vertical-align: middle;
}

.driver-badge-info {
  border-color: var(--ui-border-strong);
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

.driver-badge-warning {
  border-color: color-mix(in srgb, var(--ui-warning) 36%, transparent);
  background: color-mix(in srgb, var(--ui-warning) 12%, transparent);
  color: var(--ui-warning-text);
}

/* 驱动管理面板 */
.vmouse-panel {
  border-radius: var(--ui-radius-md);
  border: 1px solid var(--ui-border);
  background: var(--ui-surface);
  overflow: hidden;
  transition: border-color 0.2s ease, box-shadow 0.2s ease;
}

.vmouse-panel:hover {
  border-color: var(--ui-border-strong);
  box-shadow: var(--ui-shadow-sm);
}

/* 面板头部 */
.vmouse-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.6rem 0.85rem;
  background: var(--ui-surface-strong);
  border-bottom: 1px solid var(--ui-border);
}

/* 状态指示器 */
.vmouse-status-indicator {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.vmouse-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.vmouse-dot.dot-active {
  background: var(--ui-success);
  box-shadow: 0 0 0 4px color-mix(in srgb, var(--ui-success) 16%, transparent);
  animation: vmouse-pulse 2s ease-in-out infinite;
}

.vmouse-dot.dot-warning {
  background: var(--ui-warning);
  box-shadow: 0 0 0 4px color-mix(in srgb, var(--ui-warning) 16%, transparent);
}

.vmouse-dot.dot-inactive {
  background: var(--ui-text-muted);
}

@keyframes vmouse-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.vmouse-status-label {
  font-size: 0.8rem;
  font-weight: 500;
  color: var(--ui-text-secondary);
}

/* 刷新按钮 */
.vmouse-refresh-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: 6px;
  background: transparent;
  color: var(--ui-text-secondary);
  cursor: pointer;
  transition: all 0.15s ease;
}

.vmouse-refresh-btn:hover:not(:disabled) {
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

.vmouse-refresh-btn:focus-visible {
  outline: 2px solid var(--ui-accent);
  outline-offset: 2px;
}

.vmouse-refresh-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

/* 面板操作区 */
.vmouse-panel-body {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  padding: 0.6rem 0.85rem;
  background: var(--ui-surface);
}

/* 操作按钮 */
.vmouse-action-btn {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  padding: 0.35rem 0.85rem;
  border: 1px solid transparent;
  border-radius: 6px;
  font-size: 0.8rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.15s ease;
}

.vmouse-action-btn:disabled {
  opacity: 0.65;
  cursor: not-allowed;
}

.vmouse-install-btn {
  background: var(--ui-accent);
  color: var(--ui-accent-contrast);
  border-color: var(--ui-accent);
}

.vmouse-install-btn:hover:not(:disabled) {
  box-shadow: 0 0 0 3px var(--ui-accent-soft);
}

.vmouse-uninstall-btn {
  background: transparent;
  color: var(--ui-danger-text);
  border-color: var(--ui-danger-border);
}

.vmouse-uninstall-btn:hover:not(:disabled) {
  background: var(--ui-danger-soft);
  color: var(--ui-danger-text);
}

/* 操作中 spinner */
.vmouse-spinner {
  width: 14px;
  height: 14px;
  border: 2px solid currentColor;
  border-top-color: transparent;
  border-radius: 50%;
  animation: vmouse-spin 0.6s linear infinite;
}

@keyframes vmouse-spin {
  to { transform: rotate(360deg); }
}

#input > hr,
#input .accordion-body > hr {
  margin-block: 1.25rem;
  border-color: var(--ui-border);
  opacity: 1;
}

#input .accordion-item {
  overflow: hidden;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

#input .accordion-button {
  border: 0;
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
  font-weight: 600;
}

#input .accordion-button:hover,
#input .accordion-button:not(.collapsed) {
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

#input .accordion-button:focus {
  box-shadow: inset 0 0 0 2px var(--ui-accent-soft);
}

@media (max-width: 575.98px) {
  .vmouse-panel-body {
    flex-direction: column;
  }

  .vmouse-action-btn {
    justify-content: center;
    width: 100%;
  }
}
</style>
