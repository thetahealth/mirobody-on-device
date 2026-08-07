/**
 * The health metrics this client ingests from 华为运动健康 (Huawei Health), with
 * the FHIR coding each one maps to — the harmony counterpart of Android's
 * HealthMetric enum (android/.../data/health/HealthSample.kt).
 *
 * ONE MAPPED METRIC PER CONFIDENT (LOINC, UCUM) PAIR. Health Service Kit exposes
 * far more than this (HRV, stress, emotion, skin temperature, ~200 workout types,
 * body-composition fields); a reading is mapped only when its code AND its unit
 * are unambiguous, which is the same rule src/health/vendor_fhir.hpp follows for
 * the vendor clients. Deliberately deferred, with the reason:
 *
 *   | not mapped            | why |
 *   | calories, distance    | the SDK documents daily-activity calorie as "in calories" without saying kcal vs cal, and a wrong energy unit in a health record is worse than no record |
 *   | height                | HealthFields.Height states no unit at all (m vs cm decides a 100x error) |
 *   | HRV, stress, emotion  | no LOINC code we can defend; vendor_fhir.hpp defers the same ones |
 *   | daily HR mean         | aggregateData's HeartRateAggregation offers max/min/last but NOT avg, and coding a daily max as plain 8867-4 would read back as a spot measurement |
 *   | workouts              | ExerciseSequence is a large surface of its own (per-sport summaries + detail series); a later round |
 *
 * `daily` metrics are one reading per day — a day's step total, a night's sleep,
 * a resting heart rate — and carry an effectivePeriod. The rest are instantaneous
 * samples read newest-first and capped (see HealthSource.read), because a week of
 * raw heart rate is thousands of rows and a chat client has no use for them.
 */

/** One coding inside a panel Observation (blood pressure's two components). */
export interface HealthCoding {
  loinc: string;
  display: string;
}

export interface HealthMetricRecord {
  /** Stable key: the enabled-metrics preference AND the FHIR id prefix. Never renamed. */
  slug: string;
  /** Localized name for the UI. The Observation carries `display` instead (FHIR is English). */
  labelRes: Resource;
  loinc: string;
  display: string;
  /** Human unit, shown in the UI and as Observation.valueQuantity.unit. */
  unit: string;
  /** UCUM code — Observation.valueQuantity.code. */
  ucum: string;
  /** FHIR observation-category token. */
  category: string;
  /** One reading per day (aggregate / sequence) rather than an instant sample. */
  daily: boolean;
  /** Systolic component of a blood-pressure panel; set on panel metrics only. */
  systolic?: HealthCoding;
  /** Diastolic component. Both present => the mapper emits components, not valueQuantity. */
  diastolic?: HealthCoding;
}

export const HEALTH_METRICS: HealthMetricRecord[] = [
  {
    slug: 'steps',
    labelRes: $r('app.string.health_metric_steps'),
    loinc: '41950-7',
    display: 'Number of steps in 24 hour Measured',
    unit: 'steps',
    ucum: '{steps}',
    category: 'activity',
    daily: true
  },
  {
    slug: 'sleep',
    labelRes: $r('app.string.health_metric_sleep'),
    loinc: '93832-4',
    display: 'Sleep duration',
    unit: 'min',
    ucum: 'min',
    category: 'activity',
    daily: true
  },
  {
    slug: 'rhr',
    labelRes: $r('app.string.health_metric_rhr'),
    loinc: '40443-4',
    display: 'Heart rate --resting',
    unit: 'beats/minute',
    ucum: '/min',
    category: 'vital-signs',
    daily: true
  },
  {
    slug: 'hr',
    labelRes: $r('app.string.health_metric_hr'),
    loinc: '8867-4',
    display: 'Heart rate',
    unit: 'beats/minute',
    ucum: '/min',
    category: 'vital-signs',
    daily: false
  },
  {
    slug: 'spo2',
    labelRes: $r('app.string.health_metric_spo2'),
    loinc: '59408-5',
    display: 'Oxygen saturation in Arterial blood by Pulse oximetry',
    unit: '%',
    ucum: '%',
    category: 'vital-signs',
    daily: false
  },
  {
    slug: 'weight',
    labelRes: $r('app.string.health_metric_weight'),
    loinc: '29463-7',
    display: 'Body weight',
    unit: 'kg',
    ucum: 'kg',
    category: 'vital-signs',
    daily: false
  },
  {
    // The FHIR vitals pattern: a panel resource whose two children carry the
    // numbers. Mapping systolic alone would silently drop half the reading.
    slug: 'bp',
    labelRes: $r('app.string.health_metric_bp'),
    loinc: '85354-9',
    display: 'Blood pressure panel with all children optional',
    unit: 'mmHg',
    ucum: 'mm[Hg]',
    category: 'vital-signs',
    daily: false,
    systolic: { loinc: '8480-6', display: 'Systolic blood pressure' },
    diastolic: { loinc: '8462-4', display: 'Diastolic blood pressure' }
  },
  {
    slug: 'temp',
    labelRes: $r('app.string.health_metric_temp'),
    loinc: '8310-5',
    display: 'Body temperature',
    unit: '°C',
    ucum: 'Cel',
    category: 'vital-signs',
    daily: false
  }
];

/** Slugs enabled on first run: the four both a phone and a watch reliably have. */
export const DEFAULT_METRIC_SLUGS: string[] = ['steps', 'sleep', 'rhr', 'hr'];

export function findMetric(slug: string): HealthMetricRecord | undefined {
  for (const m of HEALTH_METRICS) {
    if (m.slug === slug) {
      return m;
    }
  }
  return undefined;
}

/**
 * One normalized reading. `value` is in the metric's UCUM unit; `diastolic` is
 * set only for the blood-pressure panel. Instantaneous readings have
 * start === end; daily ones span [start, end).
 *
 * `id` is the DETERMINISTIC FHIR resource id — "hw.<slug>.<startMillis>" — and it
 * is what makes a re-sync idempotent: the same reading written twice replaces its
 * own row instead of adding a second one (see mirobody_health_store in
 * src/mirobody.h). Two sources reporting the same metric at the same millisecond
 * collapse to one row; that is the intended trade for not accumulating duplicates.
 */
export interface HealthSample {
  metric: HealthMetricRecord;
  value: number;
  diastolic: number;      // 0 when the metric is not a panel
  startMillis: number;
  endMillis: number;
  id: string;
}

export function sampleId(slug: string, startMillis: number): string {
  return 'hw.' + slug + '.' + startMillis.toString();
}
