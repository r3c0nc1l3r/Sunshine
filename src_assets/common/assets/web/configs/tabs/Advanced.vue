<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from '../../Checkbox.vue'

const props = defineProps([
  'platform',
  'config',
  'global_prep_cmd'
])

const config = ref(props.config)
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="text" class="form-control" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-3">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input type="number" class="form-control" id="qp" placeholder="28" v-model="config.qp" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-3">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-3">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-3">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" class="form-select" v-model="config.av1_mode">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div class="mb-3" v-if="platform !== 'macos'">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" class="form-select" v-model="config.capture">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="wlr">wlroots</option>
            <option value="x11">X11</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
            <option value="kwin">KWin Screencast</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #windows>
            <option value="ddx">Desktop Duplication API</option>
            <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- Encoder -->
    <div class="mb-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #freebsd>
            <option value="vulkan">Vulkan</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
            <option value="vulkan">Vulkan</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>

    <!-- Drain handoff (Linux only, off by default) -->
    <template v-if="platform === 'linux'">
      <Checkbox class="mb-3"
                id="drain_handoff"
                locale-prefix="config"
                v-model="config.drain_handoff"
                default="false"
      ></Checkbox>

      <!-- Control socket: serves the close-app verb without the drain handoff (off by default) -->
      <Checkbox class="mb-3"
                id="control_socket"
                locale-prefix="config"
                v-model="config.control_socket"
                default="false"
      ></Checkbox>

      <!-- Drain handoff UDP port offset -->
      <div class="mb-3">
        <label for="drain_handoff_udp_offset" class="form-label">{{ $t('config.drain_handoff_udp_offset') }}</label>
        <input type="number" class="form-control" id="drain_handoff_udp_offset" placeholder="100" v-model="config.drain_handoff_udp_offset" />
        <div class="form-text">{{ $t('config.drain_handoff_udp_offset_desc') }}</div>
      </div>

      <!-- Drain handoff deadline -->
      <div class="mb-3">
        <label for="drain_handoff_deadline" class="form-label">{{ $t('config.drain_handoff_deadline') }}</label>
        <input type="number" min="1" max="86400" class="form-control" id="drain_handoff_deadline" placeholder="3600" v-model="config.drain_handoff_deadline" />
        <div class="form-text">{{ $t('config.drain_handoff_deadline_desc') }}</div>
      </div>

      <!-- Drain handoff ready timeout -->
      <div class="mb-3">
        <label for="drain_handoff_ready_timeout" class="form-label">{{ $t('config.drain_handoff_ready_timeout') }}</label>
        <input type="number" min="1" class="form-control" id="drain_handoff_ready_timeout" placeholder="30" v-model="config.drain_handoff_ready_timeout" />
        <div class="form-text">{{ $t('config.drain_handoff_ready_timeout_desc') }}</div>
      </div>

      <!-- Drain handoff rollback window -->
      <div class="mb-3">
        <label for="drain_handoff_rollback_window" class="form-label">{{ $t('config.drain_handoff_rollback_window') }}</label>
        <input type="number" min="1" class="form-control" id="drain_handoff_rollback_window" placeholder="60" v-model="config.drain_handoff_rollback_window" />
        <div class="form-text">{{ $t('config.drain_handoff_rollback_window_desc') }}</div>
      </div>

      <!-- Drain handoff cooldown -->
      <div class="mb-3">
        <label for="drain_handoff_cooldown" class="form-label">{{ $t('config.drain_handoff_cooldown') }}</label>
        <input type="number" min="1" class="form-control" id="drain_handoff_cooldown" placeholder="120" v-model="config.drain_handoff_cooldown" />
        <div class="form-text">{{ $t('config.drain_handoff_cooldown_desc') }}</div>
      </div>

      <!-- Drain handoff binary -->
      <div class="mb-3">
        <label for="drain_handoff_binary" class="form-label">{{ $t('config.drain_handoff_binary') }}</label>
        <input type="text" class="form-control" id="drain_handoff_binary" v-model="config.drain_handoff_binary" />
        <div class="form-text">{{ $t('config.drain_handoff_binary_desc') }}</div>
      </div>
    </template>

  </div>
</template>

<style scoped>

</style>
