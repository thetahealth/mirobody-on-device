/**
 * BYOK provider registry — the cloud lane's provider contract.
 *
 * Mirrors docs/privacy-tiers.md (cloud lanes — provider contract): each provider is fully
 * described by a record; the app ships a built-in list and lets the user pick one + paste
 * a key. `baseUrl` and `model` stay editable so power users can point at any
 * OpenAI-compatible endpoint.
 *
 * Nothing here is hard-coded at a call site — the transport reads these records only.
 */

export interface ProviderRecord {
  id: string;         // stable internal id, also the secure-store alias for its key
  label: string;      // shown in UI
  baseUrl: string;    // OpenAI-compatible base; transport appends /chat/completions
  model: string;      // default model id (editable)
  models: string[];   // curated selectable models (UI chips); free-text override still allowed
  authScheme: string; // 'bearer' | 'header:<name>'
  regionHint: string; // 'global' | 'cn' — for first-run default selection
  compatible: string; // 'openai-chat' | 'adapter:<name>'
  registerUrl: string;// where to get a key (linked from the key-entry flow)
  note: string;       // free-tier snapshot / caveats
  catalogUrl?: string;// optional website catalog page with free/popularity/deprecation metadata
  deprecationsUrl?: string;// optional official deprecations table; hides already-shutdown models
}

/**
 * 2026-07 snapshot. Free-tier limits and free model ids rotate fast — verify against each
 * provider's live console before shipping (docs/privacy-tiers.md, "Providers").
 */
export const BUILTIN_PROVIDERS: ProviderRecord[] = [
  {
    id: 'gemini',
    label: 'Google Gemini',
    baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    model: 'gemini-3.6-flash',
    models: ['gemini-3.6-flash'],
    authScheme: 'bearer',
    regionHint: 'global',
    compatible: 'openai-chat',
    registerUrl: 'https://aistudio.google.com/apikey',
    note: '10 RPM / 250 RPD, 250K TPM, 1M ctx',
    deprecationsUrl: 'https://ai.google.dev/gemini-api/docs/deprecations'
  },
  {
    id: 'zhipu',
    label: 'Zhipu GLM (CN)',
    baseUrl: 'https://open.bigmodel.cn/api/paas/v4',
    model: 'glm-4.7-flash',
    models: ['glm-4.7-flash'],
    authScheme: 'bearer',
    regionHint: 'cn',
    compatible: 'openai-chat',
    registerUrl: 'https://open.bigmodel.cn',
    note: '永久免费档,无总量上限'
  },
  {
    id: 'nvidia',
    label: 'NVIDIA NIM',
    baseUrl: 'https://integrate.api.nvidia.com/v1',
    model: 'deepseek-ai/deepseek-v4-flash',
    models: [
      'deepseek-ai/deepseek-v4-flash',
      'deepseek-ai/deepseek-v4-pro',
      'openai/gpt-oss-120b'
    ],
    authScheme: 'bearer',
    regionHint: 'global',
    compatible: 'openai-chat',
    registerUrl: 'https://build.nvidia.com',
    note: '~40 RPM; large models cost more credits',
    catalogUrl: 'https://build.nvidia.com/models?orderBy=weightPopular%3ADESC&filters=nimType%3Anim_type_preview&pageSize=96'
  },
  {
    id: 'openrouter',
    label: 'OpenRouter (Kimi K2.6 free)',
    baseUrl: 'https://openrouter.ai/api/v1',
    model: 'moonshotai/kimi-k2.6:free',
    models: ['moonshotai/kimi-k2.6:free', 'deepseek/deepseek-chat:free', 'google/gemini-2.5-flash:free'],
    authScheme: 'bearer',
    regionHint: 'global',
    compatible: 'openai-chat',
    registerUrl: 'https://openrouter.ai/keys',
    note: 'Aggregator; free :free ids rotate — do not pin'
  },
  {
    id: 'groq',
    label: 'Groq',
    baseUrl: 'https://api.groq.com/openai/v1',
    model: 'llama-3.3-70b-versatile',
    models: ['llama-3.1-8b-instant', 'llama-3.3-70b-versatile'],
    authScheme: 'bearer',
    regionHint: 'global',
    compatible: 'openai-chat',
    registerUrl: 'https://console.groq.com/keys',
    note: 'Fastest; ~30 RPM / 1000 RPD'
  },
  {
    id: 'cerebras',
    label: 'Cerebras',
    baseUrl: 'https://api.cerebras.ai/v1',
    model: 'gpt-oss-120b',
    models: ['llama-3.3-70b', 'gpt-oss-120b'],
    authScheme: 'bearer',
    regionHint: 'global',
    compatible: 'openai-chat',
    registerUrl: 'https://cloud.cerebras.ai',
    note: '1M tok/day but 5 RPM, 8K ctx'
  }
];

export function findProvider(id: string): ProviderRecord | undefined {
  for (const p of BUILTIN_PROVIDERS) {
    if (p.id === id) {
      return p;
    }
  }
  return undefined;
}

/** Region default: CN locale → Zhipu, else Gemini. Always overridable by the user. */
export function defaultProviderId(isCN: boolean): string {
  return isCN ? 'zhipu' : 'gemini';
}
