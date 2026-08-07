/**
 * HealthSource — 华为运动健康 (Huawei Health) as a read source, through
 * **Health Service Kit** (`@kit.HealthServiceKit`, API 5.0.0(12)+).
 *
 * The harmony counterpart of Android's HmsHealthSource, and the reason this
 * client can have health data at all without a server: the Kit reads out of the
 * Huawei Health store on the device, and everything downstream stays on the
 * device too (HealthRepository writes FHIR Observations into the embedded core's
 * SQLite). Nothing here is a network call.
 *
 * GATED, LIKE ITS ANDROID SIBLING. Reading health data needs (1) Health Service
 * Kit enabled for this app in AppGallery Connect with the matching data scopes
 * APPROVED (enterprise developer, privacy policy, stated purpose), (2) the
 * device's 运动健康 app installed and signed in with a Huawei ID, and (3) the user
 * granting the read authorization. Until (1) exists every call here fails with a
 * BusinessError, which is exactly what `status()` reports — the app stays
 * functional and the UI says "not authorized" instead of crashing. That is why
 * every Kit call below is wrapped: an inert source is a first-class state.
 *
 * The Kit has three read shapes, and this file has one request builder per shape:
 *   - aggregate() -> aggregateData grouped by DAY   (steps, resting heart rate)
 *   - readData(HealthSequence) -> one record per night (sleep)
 *   - points()    -> instantaneous samples (HR, SpO2, weight, BP, temperature)
 * Extraction, though, is per metric (readOne's switch): ArkTS forbids indexed
 * access on interface fields, so a `fields[name]` table is not expressible and
 * each metric must name its own field through the Kit's model type. That keeps
 * the Kit's field names HERE, leaving the FHIR-facing registry in
 * model/HealthMetrics.ets platform-neutral.
 */
import { healthStore } from '@kit.HealthServiceKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { HealthMetricRecord, HealthSample, findMetric, sampleId } from '../model/HealthMetrics';

/** Half-open window [startMillis, endMillis) in epoch milliseconds. */
export interface TimeRange {
  startMillis: number;
  endMillis: number;
}

export function lastDays(days: number): TimeRange {
  const now = new Date().getTime();
  return { startMillis: now - days * 24 * 60 * 60 * 1000, endMillis: now };
}

/** What the source can do right now — the whole gating story in one object. */
export interface HealthStatus {
  /** False when Health Service Kit is not usable on this device/build at all. */
  available: boolean;
  /** Metric slugs the user has granted read access to. */
  granted: string[];
  /** '' when fine, else "<code>: <message>" from the Kit. */
  error: string;
}

export interface HealthReadResult {
  samples: HealthSample[];
  /** First per-metric failure ("hr: 201 ..."), '' when every enabled read worked. */
  error: string;
}

/**
 * Kit DataType for a metric slug. A switch rather than a table field because the
 * constants are values, and holding one in an interface would make the whole
 * registry evaluate at import time — before healthStore is known to exist.
 */
function dataTypeFor(slug: string): healthStore.DataType | undefined {
  if (slug === 'steps') {
    return healthStore.healthDataTypes.DAILY_ACTIVITIES;
  }
  if (slug === 'sleep') {
    return healthStore.healthDataTypes.SLEEP_RECORD;
  }
  if (slug === 'rhr') {
    return healthStore.healthDataTypes.RESTING_HEART_RATE;
  }
  if (slug === 'hr') {
    return healthStore.healthDataTypes.HEART_RATE;
  }
  if (slug === 'spo2') {
    return healthStore.healthDataTypes.BLOOD_OXYGEN_SATURATION;
  }
  if (slug === 'weight') {
    return healthStore.healthDataTypes.WEIGHT;
  }
  if (slug === 'bp') {
    return healthStore.healthDataTypes.BLOOD_PRESSURE;
  }
  if (slug === 'temp') {
    return healthStore.healthDataTypes.BODY_TEMPERATURE;
  }
  return undefined;
}

/**
 * "<code>: <message>" out of whatever the Kit threw. The code matters more than
 * the text here: an entitlement that has not been approved shows up as a code,
 * and that is the one thing a developer needs off a user's screen.
 */
function errText(e: BusinessError): string {
  if (e === undefined || e === null) {
    return 'unknown error';
  }
  const msg: string = e.message !== undefined ? e.message : '';
  return e.code !== undefined ? e.code.toString() + ': ' + msg : msg;
}

/** A Kit field value as a number; NaN when absent or non-numeric. */
function num(v: healthStore.HealthValueType): number {
  return typeof v === 'number' ? v : NaN;
}

/**
 * 'MM/DD/YYYY' in LOCAL time — the format aggregateData's startLocalDate /
 * endLocalDate document (DataRequest in @hms.health.store.d.ts). Padded rather
 * than Intl-formatted: Intl 'en-US' drops the leading zeros, and a day boundary
 * is not worth risking on a parser we can't see.
 */
function localDate(millis: number): string {
  const d = new Date(millis);
  const mm = d.getMonth() + 1;
  const dd = d.getDate();
  return (mm < 10 ? '0' + mm : mm.toString()) + '/' +
    (dd < 10 ? '0' + dd : dd.toString()) + '/' + d.getFullYear().toString();
}

export class HealthSource {
  /**
   * Recorded as Observation.method.coding[0].display — the provenance of every
   * reading this source produces. Not localized: it is data, not UI text.
   */
  static readonly SOURCE_NAME: string = 'Huawei Health (Health Service Kit)';

  private static inited: boolean = false;
  private static initError: string = '';

  /**
   * healthStore.init() once per process. Its failure IS the availability answer:
   * on a device without Health Service Kit (or a build without the entitlement)
   * it throws, and every later call would too.
   */
  private static async ensureInit(ctx: common.UIAbilityContext): Promise<boolean> {
    if (HealthSource.inited) {
      return true;
    }
    // Ask before trying: on a device with no Health Service Kit this is a
    // definite "no", where a thrown BusinessError would be indistinguishable from
    // an entitlement that has not been approved yet.
    if (!canIUse('SystemCapability.Health.HealthStore')) {
      HealthSource.initError = 'SystemCapability.Health.HealthStore is absent on this device';
      return false;
    }
    try {
      await healthStore.init(ctx);
      HealthSource.inited = true;
      HealthSource.initError = '';
      return true;
    } catch (e) {
      HealthSource.initError = errText(e as BusinessError);
      console.warn('HealthSource: init failed: ' + HealthSource.initError);
      return false;
    }
  }

  private static typesFor(slugs: string[]): healthStore.DataType[] {
    const types: healthStore.DataType[] = [];
    for (const slug of slugs) {
      const t = dataTypeFor(slug);
      if (t !== undefined) {
        types.push(t);
      }
    }
    return types;
  }

  /** Granted read types -> the slugs they correspond to (DataType.id is the identity). */
  private static slugsFor(types: healthStore.DataType[], candidates: string[]): string[] {
    const ids = new Set<number>();
    for (const t of types) {
      ids.add(t.id);
    }
    const out: string[] = [];
    for (const slug of candidates) {
      const t = dataTypeFor(slug);
      if (t !== undefined && ids.has(t.id)) {
        out.push(slug);
      }
    }
    return out;
  }

  /**
   * Can we read, and what of `slugs` is already granted? Never prompts — this is
   * what the settings card shows on open, so it must be silent.
   */
  static async status(ctx: common.UIAbilityContext, slugs: string[]): Promise<HealthStatus> {
    if (!await HealthSource.ensureInit(ctx)) {
      return { available: false, granted: [], error: HealthSource.initError };
    }
    const req: healthStore.AuthorizationRequest = {
      readDataTypes: HealthSource.typesFor(slugs),
      writeDataTypes: []
    };
    try {
      const resp: healthStore.AuthorizationResponse = await healthStore.getAuthorizations(req);
      return { available: true, granted: HealthSource.slugsFor(resp.readDataTypes, slugs), error: '' };
    } catch (e) {
      // Reachable Kit, unreadable data: entitlement missing, no Huawei ID signed
      // in, or nothing granted yet. Available stays true so the UI offers the
      // authorize button rather than declaring the feature absent.
      return { available: true, granted: [], error: errText(e as BusinessError) };
    }
  }

  /**
   * Show Huawei's own authorization sheet for `slugs` and report what came back
   * granted. Read-only: writeDataTypes stays empty, so the app can never push
   * anything into the user's Huawei Health record.
   */
  static async authorize(ctx: common.UIAbilityContext, slugs: string[]): Promise<HealthStatus> {
    if (!await HealthSource.ensureInit(ctx)) {
      return { available: false, granted: [], error: HealthSource.initError };
    }
    const req: healthStore.AuthorizationRequest = {
      readDataTypes: HealthSource.typesFor(slugs),
      writeDataTypes: []
    };
    try {
      const resp: healthStore.AuthorizationResponse = await healthStore.requestAuthorizations(ctx, req);
      return { available: true, granted: HealthSource.slugsFor(resp.readDataTypes, slugs), error: '' };
    } catch (e) {
      return { available: true, granted: [], error: errText(e as BusinessError) };
    }
  }

  /**
   * Read `slugs` over `range`. `cap` bounds the instantaneous reads (newest
   * first): a week of raw heart rate is thousands of samples, and every one
   * would become a row in the on-device FHIR store.
   *
   * A metric that fails is SKIPPED, not fatal — one revoked scope must not cost
   * the user the other seven metrics. The first failure is reported so the UI can
   * show it.
   */
  static async read(ctx: common.UIAbilityContext, range: TimeRange,
                    slugs: string[], cap: number): Promise<HealthReadResult> {
    if (!await HealthSource.ensureInit(ctx)) {
      return { samples: [], error: HealthSource.initError };
    }

    const out: HealthSample[] = [];
    let error = '';
    for (const slug of slugs) {
      const metric: HealthMetricRecord | undefined = findMetric(slug);
      if (metric === undefined) {
        continue;
      }
      try {
        const got: HealthSample[] = await HealthSource.readOne(metric, range, cap);
        for (const s of got) {
          out.push(s);
        }
      } catch (e) {
        const text = slug + ': ' + errText(e as BusinessError);
        console.warn('HealthSource: read failed, ' + text);
        if (error.length === 0) {
          error = text;
        }
      }
    }
    return { samples: out, error };
  }

  /**
   * One metric's readings. A switch with a typed reader per metric rather than a
   * field-name table, because ArkTS forbids indexed access on interface fields
   * (arkts-no-props-by-index): `p.fields[name]` does not compile, so each metric's
   * field has to be reached by its real name through the Kit's own model type.
   * The request building is still shared — points() / aggregate() below.
   */
  private static async readOne(metric: HealthMetricRecord, range: TimeRange,
                               cap: number): Promise<HealthSample[]> {
    const out: HealthSample[] = [];

    if (metric.slug === 'steps') {
      // 'sum' is the ONLY scope DailyActivitiesAggregation offers for step.
      const rows = await HealthSource.aggregate<healthStore.healthModels.DailyActivitiesAggregateResult>(
        healthStore.healthDataTypes.DAILY_ACTIVITIES, 'step', 'sum', range);
      for (const r of rows) {
        HealthSource.pushDaily(out, metric, num(r.fields.step.sum), r);
      }
      return out;
    }

    if (metric.slug === 'rhr') {
      // ...and 'last' the only one RestingHeartRateAggregation offers for restBpm.
      const rows = await HealthSource.aggregate<healthStore.healthModels.RestingHeartRateAggregateResult>(
        healthStore.healthDataTypes.RESTING_HEART_RATE, 'restBpm', 'last', range);
      for (const r of rows) {
        HealthSource.pushDaily(out, metric, num(r.fields.restBpm.last), r);
      }
      return out;
    }

    if (metric.slug === 'sleep') {
      return HealthSource.readSleep(metric, range, cap);
    }

    if (metric.slug === 'hr') {
      const rows = await HealthSource.points<healthStore.healthModels.HeartRate>(
        healthStore.healthDataTypes.HEART_RATE, range, cap);
      for (const p of rows) {
        HealthSource.pushInstant(out, metric, num(p.fields.bpm), 0, p);
      }
      return out;
    }

    if (metric.slug === 'spo2') {
      const rows = await HealthSource.points<healthStore.healthModels.BloodOxygenSaturation>(
        healthStore.healthDataTypes.BLOOD_OXYGEN_SATURATION, range, cap);
      for (const p of rows) {
        HealthSource.pushInstant(out, metric, num(p.fields.spo2), 0, p);
      }
      return out;
    }

    if (metric.slug === 'weight') {
      const rows = await HealthSource.points<healthStore.healthModels.Weight>(
        healthStore.healthDataTypes.WEIGHT, range, cap);
      for (const p of rows) {
        HealthSource.pushInstant(out, metric, num(p.fields.bodyWeight), 0, p);
      }
      return out;
    }

    if (metric.slug === 'bp') {
      const rows = await HealthSource.points<healthStore.healthModels.BloodPressure>(
        healthStore.healthDataTypes.BLOOD_PRESSURE, range, cap);
      for (const p of rows) {
        // Both halves or neither: half a blood pressure is not a reading.
        const dia: number = num(p.fields.bloodPressureDiastolic);
        if (Number.isNaN(dia)) {
          continue;
        }
        HealthSource.pushInstant(out, metric, num(p.fields.bloodPressureSystolic), dia, p);
      }
      return out;
    }

    if (metric.slug === 'temp') {
      const rows = await HealthSource.points<healthStore.healthModels.BodyTemperature>(
        healthStore.healthDataTypes.BODY_TEMPERATURE, range, cap);
      for (const p of rows) {
        HealthSource.pushInstant(out, metric, num(p.fields.bodyTemperature), 0, p);
      }
      return out;
    }

    return out;   // a metric in the registry with no reader yet
  }

  /** aggregateData grouped by calendar DAY — one value per day. */
  private static async aggregate<T extends healthStore.AggregateResult>(
      type: healthStore.DataType, field: string,
      scope: healthStore.AggregateMetricScope, range: TimeRange): Promise<T[]> {
    // `metrics` is a Record, which IS indexable in ArkTS -- unlike a result's
    // `fields`, which is why extraction above is per metric and this is not.
    const metrics: Record<string, healthStore.AggregateMetricScope[]> = {};
    metrics[field] = [scope];

    const req: healthStore.AggregateRequest = {
      dataType: type,
      metrics: metrics,
      groupBy: { unitType: healthStore.GroupUnitType.DAY },
      startLocalDate: localDate(range.startMillis),
      endLocalDate: localDate(range.endMillis)
    };
    return healthStore.aggregateData<T>(req);
  }

  /** Instantaneous samples for one data type, newest first and capped. */
  private static async points<T extends healthStore.SamplePoint>(
      type: healthStore.DataType, range: TimeRange, cap: number): Promise<T[]> {
    const req: healthStore.SamplePointReadRequest = {
      samplePointDataType: type,
      startTime: range.startMillis,
      endTime: range.endMillis,
      sortOrder: healthStore.SortOrder.DESC,
      count: cap
    };
    return healthStore.readData<T>(req);
  }

  /** A day's aggregate -> a sample spanning that day. Drops a day with no value. */
  private static pushDaily(out: HealthSample[], metric: HealthMetricRecord,
                           value: number, row: healthStore.AggregateResult): void {
    if (Number.isNaN(value)) {
      return;
    }
    out.push({
      metric: metric,
      value: value,
      diastolic: 0,
      startMillis: row.startTime,
      endMillis: row.endTime,
      id: sampleId(metric.slug, row.startTime)
    });
  }

  /** An instant sample: start === end, so the Observation gets effectiveDateTime. */
  private static pushInstant(out: HealthSample[], metric: HealthMetricRecord,
                             value: number, diastolic: number,
                             point: healthStore.SamplePoint): void {
    if (Number.isNaN(value)) {
      return;
    }
    out.push({
      metric: metric,
      value: value,
      diastolic: diastolic,
      startMillis: point.startTime,
      endMillis: point.startTime,
      id: sampleId(metric.slug, point.startTime)
    });
  }

  /**
   * One record per night. summaries.duration is SECONDS (the Kit's unit) and the
   * Observation is minutes (LOINC 93832-4), so the /60 here is the whole unit
   * conversion for this metric.
   */
  private static async readSleep(metric: HealthMetricRecord, range: TimeRange,
                                 cap: number): Promise<HealthSample[]> {
    const req: healthStore.HealthSequenceReadRequest = {
      healthSequenceDataType: healthStore.healthDataTypes.SLEEP_RECORD,
      startTime: range.startMillis,
      endTime: range.endMillis,
      sortOrder: healthStore.SortOrder.DESC,
      count: cap
    };
    const rows: healthStore.healthModels.SleepRecord[] =
      await healthStore.readData<healthStore.healthModels.SleepRecord>(req);

    const out: HealthSample[] = [];
    for (const r of rows) {
      const seconds: number = num(r.summaries.duration);
      if (Number.isNaN(seconds) || seconds <= 0) {
        continue;
      }
      // Prefer the record's own asleep/wake instants; fall back to the sequence
      // window when a device did not report them.
      const start: number = num(r.summaries.fallAsleepTime) > 0 ? r.summaries.fallAsleepTime : r.startTime;
      const end: number = num(r.summaries.wakeupTime) > 0 ? r.summaries.wakeupTime : r.endTime;
      out.push({
        metric: metric,
        value: seconds / 60,
        diastolic: 0,
        startMillis: start,
        endMillis: end,
        id: sampleId(metric.slug, start)
      });
    }
    return out;
  }

}
