<template>
  <div class="modal fade" id="editAppModal" tabindex="-1" aria-labelledby="editAppModalLabel" ref="modalElement">
    <div class="modal-dialog modal-xl">
      <div class="modal-content">
        <div class="modal-header">
          <h5 class="modal-title" id="editAppModalLabel">
            <i class="fas fa-edit me-2"></i>
            {{ isNewApp ? t('apps.add_new') : t('apps.edit') }}
          </h5>
          <button type="button" class="btn-close" data-bs-dismiss="modal" aria-label="Close"></button>
        </div>
        <div class="modal-body">
          <input type="file" ref="fileInput" style="display: none" />
          <input type="file" ref="dirInput" style="display: none" webkitdirectory />

          <form v-if="formData" @submit.prevent="saveApp">
            <div class="accordion" id="appFormAccordion">
              <AccordionItem
                id="basicInfo"
                icon="fa-info-circle"
                :title="t('apps.basic_info')"
                parent-id="appFormAccordion"
                :show="true"
              >
                <FormField
                  id="appName"
                  :label="t('apps.app_name')"
                  :hint="t('apps.app_name_desc')"
                  :validation="validation.name"
                  :value="formData.name"
                  required
                >
                  <input
                    type="text"
                    class="form-control form-control-enhanced"
                    id="appName"
                    v-model="formData.name"
                    :class="getFieldClass('name')"
                    @blur="validateField('name')"
                    required
                  />
                </FormField>

                <FormField
                  id="appOutput"
                  :label="t('apps.output_name')"
                  :hint="t('apps.output_desc')"
                  :validation="validation.output"
                >
                  <input
                    type="text"
                    class="form-control form-control-enhanced monospace"
                    id="appOutput"
                    v-model="formData.output"
                    :class="getFieldClass('output')"
                    @blur="validateField('output')"
                  />
                </FormField>

                <FormField
                  id="appCmd"
                  :label="t('apps.cmd')"
                  :validation="validation.cmd"
                  :value="formData.cmd"
                >
                  <template #default>
                    <div class="input-group">
                      <input
                        type="text"
                        class="form-control form-control-enhanced monospace"
                        id="appCmd"
                        v-model="formData.cmd"
                        :class="getFieldClass('cmd')"
                        @blur="validateField('cmd')"
                        @input="handleCmdInput"
                        :placeholder="getPlaceholderText('cmd')"
                      />
                      <button
                        class="btn btn-outline-secondary"
                        type="button"
                        @click="selectFile('cmd')"
                        :title="getButtonTitle('file')"
                      >
                        <i class="fas fa-folder-open"></i>
                      </button>
                    </div>
                  </template>
                  <template #hint>
                    {{ t('apps.cmd_desc') }}<br />
                    <strong>{{ t('_common.note') }}</strong> {{ t('apps.cmd_note') }}<br />
                    <div class="cmd-examples">
                      <div class="cmd-examples-header"><i class="fas fa-lightbulb me-1"></i>{{ t('apps.cmd_examples_title') }}</div>
                      <div class="cmd-examples-tags">
                        <span class="cmd-tag">
                          <code>cmd /c "start xbox:"</code>
                          <span class="cmd-tag-desc">Xbox Game</span>
                        </span>
                        <span class="cmd-tag">
                          <code>steam://open/bigpicture</code>
                          <span class="cmd-tag-desc">Steam Big Picture</span>
                        </span>
                        <span class="cmd-tag">
                          <code>cmd /c "start ms-gamebar:"</code>
                          <span class="cmd-tag-desc">Xbox Game Bar</span>
                        </span>
                        <span class="cmd-tag">
                          <code>cmd /c "start playnite://playnite/showMainWindow"</code>
                          <span class="cmd-tag-desc">Playnite</span>
                        </span>
                        <span class="cmd-tag">
                          <code>"C:\Program Files\...\game.exe"</code>
                          <span class="cmd-tag-desc">Start program directly</span>
                        </span>
                      </div>
                    </div>
                  </template>
                </FormField>

                <FormField
                  id="appWorkingDir"
                  :label="t('apps.working_dir')"
                  :hint="t('apps.working_dir_desc')"
                  :validation="validation['working-dir']"
                >
                  <div class="input-group">
                    <input
                      type="text"
                      class="form-control form-control-enhanced monospace"
                      id="appWorkingDir"
                      v-model="formData['working-dir']"
                      :class="getFieldClass('working-dir')"
                      @blur="validateField('working-dir')"
                      :placeholder="getPlaceholderText('working-dir')"
                    />
                    <button
                      class="btn btn-outline-secondary"
                      type="button"
                      @click="selectDirectory('working-dir')"
                      :title="getButtonTitle('directory')"
                    >
                      <i class="fas fa-folder-open"></i>
                    </button>
                  </div>
                </FormField>
              </AccordionItem>

              <AccordionItem id="commands" icon="fa-terminal" :title="t('apps.command_settings')" parent-id="appFormAccordion">
                <div class="form-group-enhanced">
                  <div class="form-check form-switch">
                    <input
                      type="checkbox"
                      class="form-check-input"
                      id="excludeGlobalPrepSwitch"
                      v-model="formData['exclude-global-prep-cmd']"
                      :true-value="'true'"
                      :false-value="'false'"
                    />
                    <label class="form-check-label" for="excludeGlobalPrepSwitch">
                      {{ t('apps.global_prep_name') }}
                    </label>
                  </div>
                  <div class="field-hint">{{ t('apps.global_prep_desc') }}</div>
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.cmd_prep_name') }}</label>
                  <div class="field-hint mb-3">{{ t('apps.cmd_prep_desc') }}</div>
                  <CommandTable
                    :commands="formData['prep-cmd']"
                    :platform="platform"
                    type="prep"
                    @add-command="addPrepCommand"
                    @remove-command="removePrepCommand"
                    @order-changed="handlePrepCommandOrderChanged"
                  />
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.menu_cmd_name') }}</label>
                  <div class="field-hint mb-3">{{ t('apps.menu_cmd_desc') }}</div>
                  <CommandTable
                    :commands="formData['menu-cmd']"
                    :platform="platform"
                    type="menu"
                    @add-command="addMenuCommand"
                    @remove-command="removeMenuCommand"
                    @test-command="testMenuCommand"
                    @order-changed="handleMenuCommandOrderChanged"
                  />
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.detached_cmds') }}</label>
                  <div class="field-hint mb-3">
                    {{ t('apps.detached_cmds_desc') }}<br>
                    <strong>{{ t('_common.note') }}</strong> {{ t('apps.detached_cmds_note') }}
                  </div>
                  <CommandTable
                    :commands="formData.detached"
                    :platform="platform"
                    type="detached"
                    @add-command="addDetachedCommand"
                    @remove-command="removeDetachedCommand"
                    @order-changed="handleDetachedCommandOrderChanged"
                  />
                </div>
              </AccordionItem>

              <AccordionItem
                v-if="isWindows"
                id="rtxHdr"
                icon="fa-sun"
                :title="t('apps.rtx_hdr')"
                parent-id="appFormAccordion"
              >
                <FormField id="appRtxHdrMode" :label="t('apps.rtx_hdr_mode')" :hint="t('apps.rtx_hdr_desc')">
                  <select id="appRtxHdrMode" class="form-select form-control-enhanced" v-model="formData['rtx-hdr'].mode">
                    <option value="inherit">{{ t('apps.rtx_hdr_inherit') }}</option>
                    <option value="on">{{ t('apps.rtx_hdr_on') }}</option>
                    <option value="off">{{ t('apps.rtx_hdr_off') }}</option>
                  </select>
                </FormField>

                <template v-if="formData['rtx-hdr'].mode === 'on'">
                  <FormField id="appRtxHdrContrast" :label="t('apps.rtx_hdr_contrast')">
                    <input id="appRtxHdrContrast" type="number" min="-100" max="100" class="form-control form-control-enhanced" v-model.number="formData['rtx-hdr'].contrast" />
                  </FormField>
                  <FormField id="appRtxHdrSaturation" :label="t('apps.rtx_hdr_saturation')">
                    <input id="appRtxHdrSaturation" type="number" min="-100" max="100" class="form-control form-control-enhanced" v-model.number="formData['rtx-hdr'].saturation" />
                  </FormField>
                  <FormField id="appRtxHdrMiddleGray" :label="t('apps.rtx_hdr_middle_gray')">
                    <input id="appRtxHdrMiddleGray" type="number" min="10" max="100" class="form-control form-control-enhanced" v-model.number="formData['rtx-hdr']['middle-gray']" />
                  </FormField>
                  <FormField id="appRtxHdrPeakNits" :label="t('apps.rtx_hdr_peak_nits')">
                    <input id="appRtxHdrPeakNits" type="number" min="400" max="1000" step="50" class="form-control form-control-enhanced" v-model.number="formData['rtx-hdr']['peak-nits']" />
                  </FormField>
                </template>
              </AccordionItem>

              <AccordionItem id="advanced" icon="fa-cogs" :title="t('apps.advanced_options')" parent-id="appFormAccordion">
                <CheckboxField
                  v-if="isWindows"
                  id="appElevation"
                  v-model="formData.elevated"
                  :label="t('_common.run_as')"
                  :hint="t('apps.run_as_desc')"
                />

                <FormField
                  v-if="isWindows"
                  id="gamepadMode"
                  :label="t('apps.gamepad_mode')"
                  :hint="t('apps.gamepad_mode_desc')"
                >
                  <select
                    id="gamepadMode"
                    class="form-select form-control-enhanced"
                    v-model="formData.gamepad"
                  >
                    <option
                      v-for="mode in PER_APP_GAMEPAD_MODES"
                      :key="mode.value"
                      :value="mode.value"
                    >
                      {{ t(mode.labelKey) }}
                    </option>
                  </select>
                </FormField>

                <div
                  v-if="isWindows && formData.gamepad === 'ds5'"
                  class="alert alert-warning mt-2 mb-3"
                  role="note"
                >
                  <i class="fas fa-info-circle me-2" aria-hidden="true"></i>
                  {{ t('config.gamepad_ds5_component_hint_windows') }}
                </div>

                <FormField
                  v-if="isWindows"
                  id="mouseMode"
                  :label="t('apps.mouse_mode')"
                  :hint="t('apps.mouse_mode_desc')"
                >
                  <select
                    id="mouseMode"
                    class="form-select form-control-enhanced"
                    v-model="formData['mouse-mode']"
                  >
                    <option :value="0">{{ t('apps.mouse_mode_auto') }}</option>
                    <option :value="1">{{ t('apps.mouse_mode_vmouse') }}</option>
                    <option :value="2">{{ t('apps.mouse_mode_sendinput') }}</option>
                  </select>
                </FormField>

                <CheckboxField
                  id="autoDetach"
                  v-model="formData['auto-detach']"
                  :label="t('apps.auto_detach')"
                  :hint="t('apps.auto_detach_desc')"
                />

                <CheckboxField
                  id="waitAll"
                  v-model="formData['wait-all']"
                  :label="t('apps.wait_all')"
                  :hint="t('apps.wait_all_desc')"
                />

                <FormField
                  id="exitTimeout"
                  :label="t('apps.exit_timeout')"
                  :hint="t('apps.exit_timeout_desc')"
                  :validation="validation['exit-timeout']"
                >
                  <input
                    type="number"
                    class="form-control form-control-enhanced"
                    id="exitTimeout"
                    v-model="formData['exit-timeout']"
                    min="0"
                    :class="getFieldClass('exit-timeout')"
                    @blur="validateField('exit-timeout')"
                  />
                </FormField>
              </AccordionItem>

              <AccordionItem id="image" icon="fa-image" :title="t('apps.image_settings')" parent-id="appFormAccordion">
                <ImageSelector
                  :image-path="formData['image-path']"
                  :app-name="formData.name"
                  @update-image="updateImage"
                  @image-error="handleImageError"
                />
              </AccordionItem>
            </div>
          </form>
        </div>
        <div class="modal-footer modal-footer-enhanced">
          <div class="save-status">
            <span v-if="isFormValid" class="text-success"> <i class="fas fa-check-circle me-1"></i>{{ t('apps.form_valid') }} </span>
            <span v-else class="text-warning"> <i class="fas fa-exclamation-triangle me-1"></i>{{ t('apps.form_invalid') }} </span>
            <div v-if="imageError" class="text-danger mt-1">
              <i class="fas fa-exclamation-circle me-1"></i>{{ imageError }}
            </div>
          </div>
          <div>
            <button type="button" class="btn btn-secondary me-2" @click="closeModal">
              <i class="fas fa-times me-1"></i>{{ t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-primary" @click="saveApp" :disabled="disabled || !isFormValid">
              <i class="fas fa-save me-1"></i>{{ t('_common.save') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import { useI18n } from 'vue-i18n'
import { validateField as validateFieldHelper, validateAppForm } from '../utils/validation.js'
import { nanoid } from 'nanoid'
import CommandTable from './CommandTable.vue'
import ImageSelector from './ImageSelector.vue'
import AccordionItem from './AccordionItem.vue'
import FormField from './FormField.vue'
import CheckboxField from './CheckboxField.vue'
import { createFileSelector } from '../utils/fileSelection.js'
import { apiPostJson } from '../utils/apiFetch.js'
import { deepClone } from '../utils/helpers.js'
import { PER_APP_GAMEPAD_MODES } from '../utils/gamepadModes.js'

const DEFAULT_FORM_DATA = Object.freeze({
  name: '',
  output: '',
  cmd: '',
  index: -1,
  'exclude-global-prep-cmd': false,
  elevated: false,
  'auto-detach': true,
  'wait-all': true,
  'exit-timeout': 5,
  'mouse-mode': 0,
  gamepad: '',
  'prep-cmd': [],
  'menu-cmd': [],
  detached: [],
  'image-path': '',
  'working-dir': '',
  'rtx-hdr': {
    mode: 'inherit',
    contrast: 0,
    saturation: 0,
    'middle-gray': 50,
    'peak-nits': 1000,
  },
})

const FIELD_VALIDATION_MAP = Object.freeze({
  name: 'appName',
  cmd: 'command',
  output: 'outputName',
  'working-dir': 'workingDir',
  'exit-timeout': 'timeout',
  'image-path': 'imagePath',
})

const props = defineProps({
  app: { type: Object, default: null },
  platform: { type: String, default: 'linux' },
  disabled: { type: Boolean, default: false },
})

const emit = defineEmits(['close', 'save-app'])

const { t } = useI18n()

const modalElement = ref(null)
const fileInput = ref(null)
const dirInput = ref(null)
const formData = ref(null)
const validation = ref({})
const imageError = ref('')
const modalInstance = ref(null)
const fileSelector = ref(null)

const isWindows = computed(() => props.platform === 'windows')
const isNewApp = computed(() => !props.app || props.app.index === -1)
const isFormValid = computed(() => {
  // name 字段是必填的，必须验证通过
  const nameValid = validation.value.name?.isValid === true
  
  // cmd 字段不是必填的，如果已验证则使用验证结果，如果未验证或为空则认为有效
  const cmdValid = validation.value.cmd?.isValid !== false  // undefined 或 true 都认为有效
  
  return nameValid && cmdValid
})

const showMessage = (message, type = 'info') => {
  if (window.showToast) {
    window.showToast(message, type)
  } else if (type === 'error') {
    alert(message)
  } else {
    console.info(message)
  }
}

const initializeModal = () => {
  if (modalInstance.value || !modalElement.value) return

  const Modal = window.bootstrap?.Modal
  if (!Modal) {
    console.warn('Bootstrap Modal not available')
    return
  }

  try {
    modalInstance.value = new Modal(modalElement.value, {
      backdrop: 'static',
      keyboard: false,
    })
  } catch (error) {
    console.warn('Modal initialization failed:', error)
  }
}

const initializeFileSelector = () => {
  const notify = (type) => (message) => showMessage(message, type)
  fileSelector.value = createFileSelector({
    platform: props.platform,
    onSuccess: notify('info'),
    onError: notify('error'),
    onInfo: notify('info'),
  })
}

const ensureDefaultValues = () => {
  const arrayDefaults = ['prep-cmd', 'menu-cmd', 'detached']
  arrayDefaults.forEach((key) => {
    if (!formData.value[key]) formData.value[key] = []
  })

  if (!formData.value['exclude-global-prep-cmd']) {
    formData.value['exclude-global-prep-cmd'] = false
  }
  if (!formData.value['working-dir']) {
    formData.value['working-dir'] = ''
  }

  if (isWindows.value && formData.value.elevated === undefined) {
    formData.value.elevated = false
  }
  if (formData.value['auto-detach'] === undefined) {
    formData.value['auto-detach'] = true
  }
  if (formData.value['wait-all'] === undefined) {
    formData.value['wait-all'] = true
  }
  if (formData.value['exit-timeout'] === undefined) {
    formData.value['exit-timeout'] = 5
  }
  if (isWindows.value && formData.value['mouse-mode'] === undefined) {
    formData.value['mouse-mode'] = 0
  }
  if (isWindows.value && formData.value.gamepad === undefined) {
    formData.value.gamepad = ''
  }
  const rtxHdr = formData.value['rtx-hdr']
  formData.value['rtx-hdr'] = {
    ...DEFAULT_FORM_DATA['rtx-hdr'],
    ...(rtxHdr && typeof rtxHdr === 'object' ? rtxHdr : {}),
  }
}

const initializeForm = (app) => {
  formData.value = { ...DEFAULT_FORM_DATA, ...deepClone(app) }
  ensureDefaultValues()
  validation.value = {}
  imageError.value = ''
  // 立即验证所有字段，确保表单状态正确
  nextTick(() => {
    // 验证必填字段 name（总是验证）
    validateField('name')
    // 验证 cmd 字段（如果有值则验证，没有值则标记为有效）
    if (formData.value.cmd && formData.value.cmd.trim()) {
      validateField('cmd')
    } else {
      // cmd 字段不是必填的，如果为空则标记为有效
      validation.value.cmd = { isValid: true, message: '' }
    }
  })
}

const showModal = () => {
  if (!modalInstance.value) initializeModal()
  modalInstance.value?.show()
}

const resetFileSelection = () => {
  fileSelector.value?.resetState()
  fileSelector.value?.cleanupFileInputs(fileInput.value, dirInput.value)
}

const closeModal = () => {
  modalInstance.value?.hide()
  setTimeout(() => {
    resetFileSelection()
    emit('close')
  }, 300)
}

const cleanup = () => {
  modalInstance.value?.dispose()
  resetFileSelection()
}

const validateField = (fieldName) => {
  const validationKey = FIELD_VALIDATION_MAP[fieldName] || fieldName
  const result = validateFieldHelper(validationKey, formData.value[fieldName])
  validation.value[fieldName] = result
  return result
}

// 处理 cmd 字段输入，如果清空则立即更新验证状态
const handleCmdInput = () => {
  // 如果 cmd 字段被清空，立即标记为有效（因为不是必填字段）
  if (!formData.value.cmd || !formData.value.cmd.trim()) {
    validation.value.cmd = { isValid: true, message: '' }
  }
}

const getFieldClass = (fieldName) => {
  const v = validation.value[fieldName]
  if (!v) return ''
  return {
    'is-invalid': !v.isValid,
    'is-valid': v.isValid && formData.value[fieldName],
  }
}

const createCommand = (type) => {
  const baseCmd = type === 'prep' ? { do: '', undo: '' } : { id: nanoid(10), name: '', cmd: '' }
  if (isWindows.value) baseCmd.elevated = false
  return baseCmd
}

const addPrepCommand = () => {
  formData.value['prep-cmd'].push(createCommand('prep'))
}

const removePrepCommand = (index) => {
  formData.value['prep-cmd'].splice(index, 1)
}

const addMenuCommand = () => {
  formData.value['menu-cmd'].push(createCommand('menu'))
}

const removeMenuCommand = (index) => {
  formData.value['menu-cmd'].splice(index, 1)
}

const handlePrepCommandOrderChanged = (newOrder) => {
  formData.value['prep-cmd'] = newOrder
}

const handleMenuCommandOrderChanged = (newOrder) => {
  formData.value['menu-cmd'] = newOrder
}

const testMenuCommand = async (index) => {
  const menuCmd = formData.value['menu-cmd'][index]
  if (!menuCmd.cmd) {
    showMessage(t('apps.test_menu_cmd_empty'), 'error')
    return
  }

  try {
    showMessage(t('apps.test_menu_cmd_executing'))
    const result = await apiPostJson('/api/apps/test-menu-cmd', {
      cmd: menuCmd.cmd,
      working_dir: formData.value['working-dir'] || '',
      elevated: menuCmd.elevated === 'true' || menuCmd.elevated === true,
    })
    const isSuccess = result.status
    showMessage(
      isSuccess
        ? t('apps.test_menu_cmd_success')
        : `${t('apps.test_menu_cmd_failed')}: ${result.error || 'Unknown error'}`,
      isSuccess ? 'info' : 'error'
    )
  } catch (error) {
    showMessage(`${t('apps.test_menu_cmd_failed')}: ${error.message}`, 'error')
  }
}

const addDetachedCommand = () => {
  formData.value.detached.push('')
}

const removeDetachedCommand = (index) => {
  formData.value.detached.splice(index, 1)
}

const handleDetachedCommandOrderChanged = (newOrder) => {
  formData.value.detached = newOrder
}

const updateImage = (imagePath) => {
  formData.value['image-path'] = imagePath
  imageError.value = ''
}

const handleImageError = (error) => {
  imageError.value = error
}

const onFilePathSelected = (fieldName, filePath) => {
  formData.value[fieldName] = filePath
  validateField(fieldName)
}

const selectFile = (fieldName) => {
  if (!fileSelector.value) {
    showMessage(t('apps.file_selector_not_initialized'), 'error')
    return
  }
  fileSelector.value.selectFile(fieldName, fileInput.value, onFilePathSelected)
}

const selectDirectory = (fieldName) => {
  if (!fileSelector.value) {
    showMessage(t('apps.file_selector_not_initialized'), 'error')
    return
  }
  fileSelector.value.selectDirectory(fieldName, dirInput.value, onFilePathSelected)
}

const getPlaceholderText = (fieldName) => fileSelector.value?.getPlaceholderText(fieldName) || ''

const getButtonTitle = (type) => fileSelector.value?.getButtonTitle(type) || t('apps.select')

const saveApp = async () => {
  const formValidation = validateAppForm(formData.value)
  if (!formValidation.isValid) {
    if (formValidation.errors.length) alert(formValidation.errors[0])
    return
  }

  const editedApp = { ...formData.value }
  if (editedApp['image-path']) {
    editedApp['image-path'] = editedApp['image-path'].toString().replace(/"/g, '')
  }

  emit('save-app', editedApp)
}

watch(
  () => props.app,
  (newApp) => {
    if (newApp) {
      initializeForm(newApp)
      nextTick(showModal)
    }
  },
  { immediate: true }
)

onMounted(() => {
  nextTick(() => {
    initializeModal()
    initializeFileSelector()
  })
})

onBeforeUnmount(cleanup)
</script>

<style lang="less" scoped>
.modal-body {
  max-height: calc(100vh - 200px);
  overflow-y: auto;

  /* 滚动条美化 */
  &::-webkit-scrollbar {
    width: 6px;
  }

  &::-webkit-scrollbar-track {
    background: transparent;
    border-radius: 3px;
  }

  &::-webkit-scrollbar-thumb {
    background: var(--ui-border-strong);
    border-radius: 3px;
    transition: background 0.2s ease;

    &:hover {
      background: var(--ui-accent);
    }
  }
}

.modal-footer-enhanced {
  border-top: 1px solid var(--ui-border);
  padding: 1rem 1.5rem;
  display: flex;
  justify-content: space-between;
  align-items: center;
  
  background: var(--ui-surface);
}

.save-status {
  font-size: 0.875rem;
  color: var(--ui-text-muted);
}

.is-invalid {
  border-color: var(--ui-danger);
}

.is-valid {
  border-color: var(--ui-success);
}

.monospace {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
}

.cmd-examples {
  margin-top: 0.5rem;
  padding: 0.75rem;
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);

  &-header {
    font-size: 0.75rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
    
    color: var(--ui-accent);
  }

  &-tags {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    line-height: 1.5;
  }
}

.cmd-tag {
  display: inline-flex;
  align-items: center;
  gap: 0.375rem;
  padding: 0.375rem 0.625rem;
  border-radius: var(--ui-radius-sm);
  font-size: 0.75rem;
  transition: all 0.2s ease;
  
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);

  &:hover {
    transform: translateY(-1px);
    background: var(--ui-surface-hover);
    border-color: var(--ui-border-strong);
    box-shadow: var(--ui-shadow-sm);
  }

  code {
    font-family: monospace;
    font-size: 0.7rem;
    padding: 0.125rem 0.5rem;
    border-radius: 5px;
    border: none;
    
    background: var(--ui-accent-soft);
    color: var(--ui-accent);
  }

  &-desc {
    font-size: 0.7rem;
    white-space: nowrap;
    
    color: var(--ui-text-muted);
  }
}
</style>
