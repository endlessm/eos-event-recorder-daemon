const EosMetrics = imports.gi.EosMetrics;

if (typeof EosMetrics.EVENT_USER_LOGGED_IN !== 'string') {
  throw 'Expected EVENT_USER_LOGGED_IN to be defined and of type string.'
}
