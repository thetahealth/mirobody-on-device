/**
 * HealthRepository — 华为运动健康 readings -> FHIR Observations -> the embedded
 * core's on-device store, plus the read-back and the sync preferences.
 *
 * The harmony analogue of Android's HealthRepository, with the one structural
 * difference this client is built around: THERE IS NO SERVER TO POST TO. Android
 * reads its platform store and POSTs each Observation to /fhir; here the same
 * Observations go straight into the app's own SQLite through the NAPI bridge
 * (nativeHealthStore -> mirobody_health_store -> fhir::FhirStore), the same table
 * the server would have written. Nothing leaves the device.
 *
 * What that buys: the `family_health` MCP tool is compiled into this build and
 * runs on every native-lane turn (res/mcp_tools/family_health.cpp reads
 * Observations for the caller), so once a sync has landed the model can answer
 * "how did I sleep this week" from local data — no prompt injection, no extra
 * plumbing, and nothing about the user's health rides along in a request they
 * did not ask a health question in.
 *
 * Sync is IDEMPOTENT: every sample carries a deterministic FHIR id
 * ("hw.<metric>.<startMillis>", see model/HealthMetrics.ets), so re-syncing an
 * overlapping window replaces those readings instead of duplicating them. That is
 * what makes a plain "sync now" button safe to press twice.
 */
import mirobody from 'libmirobody.so';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { HealthSample, DEFAULT_METRIC_SLUGS } from '../model/HealthMetrics';
import { HealthSource, HealthStatus, HealthReadResult, TimeRange, lastDays } from './HealthSource';
import { NativeCore } from './NativeCore';

const STORE_NAME = 'health_config';
const KEY_METRICS = 'enabled_metrics';
const KEY_WINDOW = 'window_days';
const KEY_LAST_SYNC = 'last_sync';
const KEY_LAST_STORED = 'last_stored';

/** Newest-first cap per instantaneous metric, per sync. See HealthSource.read. */
const INSTANT_CAP = 200;

/** Default lookback. A week is what a "how am I doing" question actually spans. */
const DEFAULT_WINDOW_DAYS = 7;

// ── FHIR R4 Observation, only the members we write ─────────────────────────────
// Mirrors android/.../FhirObservation.kt so a reading looks the same in both
// clients' stores. Optional members are omitted rather than null: the core's
// validator (fhir::validate_resource) rejects a top-level null.

interface FhirCoding {
  system?: string;
  code?: string;
  display?: string;
}

interface FhirCodeableConcept {
  coding: FhirCoding[];
}

interface FhirQuantity {
  value: number;
  unit: string;
  system: string;
  code: string;
}

interface FhirPeriod {
  start: string;
  end: string;
}

interface FhirComponent {
  code: FhirCodeableConcept;
  valueQuantity: FhirQuantity;
}

interface FhirObservation {
  resourceType: string;
  /** Client-assigned and deterministic — this is the idempotency key. */
  id: string;
  status: string;
  category: FhirCodeableConcept[];
  code: FhirCodeableConcept;
  effectiveDateTime?: string;
  effectivePeriod?: FhirPeriod;
  valueQuantity?: FhirQuantity;
  component?: FhirComponent[];
  method: FhirCodeableConcept;
}

const LOINC_SYSTEM = 'http://loinc.org';
const UCUM_SYSTEM = 'http://unitsofmeasure.org';
const CATEGORY_SYSTEM = 'http://terminology.hl7.org/CodeSystem/observation-category';

/** What one sync did. `error` is '' on a clean run. */
export interface SyncOutcome {
  stored: number;
  failed: number;
  error: string;
}

/** One row of the read-back, as mirobody_health_recent reports it. */
export interface HealthEntry {
  code: string;
  display: string;
  value: number;
  unit: string;
  time: string;
  source: string;
}

export interface HealthRecent {
  total: number;
  items: HealthEntry[];
}

/** mirobody_health_store's JSON result. */
interface StoreResult {
  stored: number;
  failed: number;
  error: string;
}

function iso(millis: number): string {
  return new Date(millis).toISOString();
}

function quantity(value: number, unit: string, ucum: string): FhirQuantity {
  return { value: value, unit: unit, system: UCUM_SYSTEM, code: ucum };
}

function loincConcept(code: string, display: string): FhirCodeableConcept {
  return { coding: [{ system: LOINC_SYSTEM, code: code, display: display }] };
}

/**
 * One reading -> one Observation. An instantaneous sample carries
 * effectiveDateTime; a daily one (steps, sleep, resting HR) an effectivePeriod,
 * which is also what says "this is a day's figure, not a spot measurement".
 *
 * Blood pressure is the one panel: FHIR models it as 85354-9 with systolic and
 * diastolic components, so it gets `component` instead of `valueQuantity`.
 */
function toObservation(s: HealthSample): FhirObservation {
  const obs: FhirObservation = {
    resourceType: 'Observation',
    id: s.id,
    status: 'final',
    category: [{ coding: [{ system: CATEGORY_SYSTEM, code: s.metric.category }] }],
    code: loincConcept(s.metric.loinc, s.metric.display),
    method: { coding: [{ display: HealthSource.SOURCE_NAME }] }
  };

  if (s.startMillis === s.endMillis) {
    obs.effectiveDateTime = iso(s.startMillis);
  } else {
    obs.effectivePeriod = { start: iso(s.startMillis), end: iso(s.endMillis) };
  }

  const sys = s.metric.systolic;
  const dia = s.metric.diastolic;
  if (sys !== undefined && dia !== undefined) {
    obs.component = [
      {
        code: loincConcept(sys.loinc, sys.display),
        valueQuantity: quantity(s.value, s.metric.unit, s.metric.ucum)
      },
      {
        code: loincConcept(dia.loinc, dia.display),
        valueQuantity: quantity(s.diastolic, s.metric.unit, s.metric.ucum)
      }
    ];
  } else {
    obs.valueQuantity = quantity(s.value, s.metric.unit, s.metric.ucum);
  }
  return obs;
}

export class HealthRepository {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (HealthRepository.store === null) {
      HealthRepository.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return HealthRepository.store;
  }

  static async enabledSlugs(ctx: common.UIAbilityContext): Promise<string[]> {
    const p = await HealthRepository.prefs(ctx);
    const raw = await p.get(KEY_METRICS, JSON.stringify(DEFAULT_METRIC_SLUGS)) as string;
    try {
      return JSON.parse(raw) as string[];
    } catch (err) {
      return DEFAULT_METRIC_SLUGS;
    }
  }

  static async setEnabledSlugs(ctx: common.UIAbilityContext, slugs: string[]): Promise<void> {
    const p = await HealthRepository.prefs(ctx);
    await p.put(KEY_METRICS, JSON.stringify(slugs));
    await p.flush();
  }

  static async windowDays(ctx: common.UIAbilityContext): Promise<number> {
    const p = await HealthRepository.prefs(ctx);
    return await p.get(KEY_WINDOW, DEFAULT_WINDOW_DAYS) as number;
  }

  static async setWindowDays(ctx: common.UIAbilityContext, days: number): Promise<void> {
    const p = await HealthRepository.prefs(ctx);
    await p.put(KEY_WINDOW, days);
    await p.flush();
  }

  /** Epoch ms of the last successful sync, 0 when never synced. */
  static async lastSync(ctx: common.UIAbilityContext): Promise<number> {
    const p = await HealthRepository.prefs(ctx);
    return await p.get(KEY_LAST_SYNC, 0) as number;
  }

  /** Observations stored by that last sync (for the card's subtitle). */
  static async lastStored(ctx: common.UIAbilityContext): Promise<number> {
    const p = await HealthRepository.prefs(ctx);
    return await p.get(KEY_LAST_STORED, 0) as number;
  }

  /** Authorization state for the currently enabled metrics. Never prompts. */
  static async status(ctx: common.UIAbilityContext): Promise<HealthStatus> {
    return HealthSource.status(ctx, await HealthRepository.enabledSlugs(ctx));
  }

  /** Show Huawei's authorization sheet for the enabled metrics. */
  static async authorize(ctx: common.UIAbilityContext): Promise<HealthStatus> {
    return HealthSource.authorize(ctx, await HealthRepository.enabledSlugs(ctx));
  }

  /**
   * Read the enabled + granted metrics over the configured window and store them
   * locally. Only GRANTED metrics are read: asking for a revoked scope fails the
   * whole Kit call on some builds, and a partial sync beats no sync.
   */
  static async sync(ctx: common.UIAbilityContext): Promise<SyncOutcome> {
    // The native store needs SQLITE_PATH / SQL_DIR injected and the DDL
    // extracted; ensureReady() is that, and is idempotent.
    await NativeCore.ensureReady(ctx);

    const enabled: string[] = await HealthRepository.enabledSlugs(ctx);
    if (enabled.length === 0) {
      return { stored: 0, failed: 0, error: 'no metrics selected' };
    }

    const st: HealthStatus = await HealthSource.status(ctx, enabled);
    if (!st.available) {
      return { stored: 0, failed: 0, error: st.error };
    }
    if (st.granted.length === 0) {
      return { stored: 0, failed: 0, error: st.error.length > 0 ? st.error : 'not authorized' };
    }

    const days: number = await HealthRepository.windowDays(ctx);
    const range: TimeRange = lastDays(days);
    const read: HealthReadResult = await HealthSource.read(ctx, range, st.granted, INSTANT_CAP);
    if (read.samples.length === 0) {
      return { stored: 0, failed: 0, error: read.error };
    }

    const resources: FhirObservation[] = [];
    for (const s of read.samples) {
      resources.push(toObservation(s));
    }

    let res: StoreResult = { stored: 0, failed: 0, error: '' };
    try {
      const json: string = await mirobody.nativeHealthStore(JSON.stringify(resources));
      res = JSON.parse(json) as StoreResult;
    } catch (err) {
      return { stored: 0, failed: resources.length, error: JSON.stringify(err) };
    }

    if (res.stored > 0) {
      const p = await HealthRepository.prefs(ctx);
      await p.put(KEY_LAST_SYNC, new Date().getTime());
      await p.put(KEY_LAST_STORED, res.stored);
      await p.flush();
    }
    // A per-metric read failure is worth surfacing even when the write succeeded:
    // "12 stored" with one scope silently missing is a lie by omission.
    const error = res.error.length > 0 ? res.error : read.error;
    return { stored: res.stored, failed: res.failed, error };
  }

  /** The most recent Observations already in the on-device store. */
  static async recent(ctx: common.UIAbilityContext, count: number): Promise<HealthRecent> {
    await NativeCore.ensureReady(ctx);
    try {
      const json: string = await mirobody.nativeHealthRecent(count);
      return JSON.parse(json) as HealthRecent;
    } catch (err) {
      console.warn('HealthRepository: recent failed: ' + JSON.stringify(err));
      return { total: 0, items: [] };
    }
  }
}
