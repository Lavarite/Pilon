import assert from 'node:assert/strict';
import test from 'node:test';
import {dayEvents, timelineEvents, parseDate, iso, startOfWeek, addDays, eventAccessibleName, publicationStatus, shareToken, eventKey, currentEvent, refreshCalendar} from './calendar.js';
const event = (id, start, end) => ({id, displayDate: '2026-10-02', displayEndDate: '2026-10-02', startMinute: start, endMinute: end});
test('adjacent events keep one lane and overlapping events share lanes', () => {
  const adjacent = timelineEvents([event('a', 540, 600), event('b', 600, 660)], '2026-10-02');
  assert.deepEqual(adjacent.map(e => e.lanes), [1, 1]);
  const overlapping = timelineEvents([event('a', 540, 600), event('b', 570, 630), event('c', 630, 660)], '2026-10-02');
  assert.deepEqual(overlapping.map(e => e.lanes), [2, 2, 1]);
  assert.deepEqual(overlapping.map(e => e.lane), [0, 1, 0]);
});
test('a block can widen through a free lane up to the next collision', () => {
  const values = [event('a', 0, 100), event('b', 5, 30), event('c', 6, 120), event('d', 102, 124)];
  const result = timelineEvents(values, '2026-10-02');
  assert.deepEqual(result.map(e => [e.id, e.lane, e.lanes, e.span]),
    [['a', 0, 3, 1], ['b', 1, 3, 1], ['c', 2, 3, 1], ['d', 0, 3, 2]]);
});
test('midnight end dates do not add the event to the next day', () => {
  const value = {...event('a', 1380, 0), displayEndDate: '2026-10-03'};
  assert.equal(dayEvents([value], '2026-10-03').length, 0);
  assert.equal(timelineEvents([value], '2026-10-02')[0].segmentEnd, 1440);
});
test('week starts and daylight changes retain calendar dates', () => {
  assert.equal(iso(startOfWeek(parseDate('2026-10-02'), 1)), '2026-09-28');
  assert.equal(iso(startOfWeek(parseDate('2026-10-02'), 7)), '2026-09-27');
  assert.equal(iso(addDays(parseDate('2026-10-24'), 1)), '2026-10-25');
});
test('event names distinguish permitted titles without exposing withheld titles', () => {
  const value = {...event('a', 540, 600), title: 'Private appointment'};
  assert.match(eventAccessibleName(value, ['title']), /Private appointment/);
  assert.match(eventAccessibleName(value, ['title']), /09:00/);
  assert.doesNotMatch(eventAccessibleName(value, []), /Private appointment/);
  assert.notEqual(eventAccessibleName(value, ['title']), eventAccessibleName({...value, title: 'Other appointment'}, ['title']));
});
test('the repeated hour identifies both offsets without changing ordinary event labels', () => {
  const value = {...event('a', 110, 70), displayDate: '2026-10-25', displayEndDate: '2026-10-25',
    start: '2026-10-25T01:50:00+01:00', end: '2026-10-25T01:10:00+00:00'};
  const [segment] = timelineEvents([value], '2026-10-25');
  assert.match(eventAccessibleName(value, []), /01:50 UTC\+01:00–01:10 UTC\+00:00/);
  assert.match(eventAccessibleName(segment, []), /01:50 UTC\+01:00–01:10 UTC\+00:00/);
  assert.match(eventAccessibleName({...value, endMinute: 130, end: '2026-10-25T02:10:00+01:00'}, []), /01:50–02:10$/);
});
test('publication status shows the server time and handles legacy publications', () => {
  assert.match(publicationStatus('2026-10-31T18:30:00Z'), /Last published/);
  assert.match(publicationStatus('2026-10-31T18:30:00Z'), /18:30/);
  assert.match(publicationStatus('2026-10-31T18:30:00Z'), /UTC/);
  assert.equal(publicationStatus(null), 'Publication time unavailable.');
  assert.equal(publicationStatus('invalid'), 'Publication time unavailable.');
});
test('share fragments only identify complete tokens on the calendar route', () => {
  const token = 'n'.repeat(43);
  assert.equal(shareToken('/calendar', '#' + token), token);
  assert.equal(shareToken('/calendar/', '#' + token), token);
  assert.equal(shareToken('/calendar', '#short'), null);
  assert.equal(shareToken('/calendar', '#' + token + '\n'), null);
  assert.equal(shareToken('/calendar', '#' + '/'.repeat(43)), null);
  assert.equal(shareToken('/other', '#' + token), null);
  assert.equal(shareToken('/calendar/nested', '#' + token), null);
});
test('the open detail is resolved from the latest publication', () => {
  const old = {...event('a', 540, 600), occurrence: 'first', title: 'Initial'};
  const updated = {...old, title: 'Updated'};
  const other = {...old, occurrence: 'second'};
  const selected = eventKey(old);
  assert.equal(currentEvent([other, updated], selected), updated);
  assert.equal(currentEvent([other], selected), null);
});
test('a calendar refresh waits for its previous request and stops on abort', async t => {
  t.mock.timers.enable({apis: ['setTimeout']});
  const controller = new AbortController();
  const requests = [];
  const stop = refreshCalendar(() => new Promise(resolve => requests.push(resolve)), controller.signal);
  assert.equal(requests.length, 1);
  t.mock.timers.tick(180000);
  assert.equal(requests.length, 1);
  requests[0]();
  await Promise.resolve();
  t.mock.timers.tick(60000);
  assert.equal(requests.length, 2);
  controller.abort(); stop();
  requests[1]();
  await Promise.resolve();
  t.mock.timers.tick(180000);
  assert.equal(requests.length, 2);
});
test('a stalled calendar request times out and polling resumes with a fresh signal', async t => {
  t.mock.timers.enable({apis: ['setTimeout']});
  const controller = new AbortController();
  const requests = [];
  const stop = refreshCalendar(signal => new Promise(resolve => {
    requests.push(signal);
    signal?.addEventListener('abort', resolve, {once: true});
  }), controller.signal);
  try {
    t.mock.timers.tick(14999);
    assert.equal(requests[0]?.aborted, false);
    t.mock.timers.tick(1);
    assert.equal(requests[0].reason.name, 'TimeoutError');
    await Promise.resolve();
    t.mock.timers.tick(60000);
    assert.equal(requests.length, 2);
    assert.equal(requests[1].aborted, false);
    controller.abort();
    assert.equal(requests[1].reason.name, 'AbortError');
  } finally {
    controller.abort();
    stop();
  }
});
