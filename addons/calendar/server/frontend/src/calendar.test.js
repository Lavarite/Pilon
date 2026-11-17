import assert from 'node:assert/strict';
import test from 'node:test';
import {dayEvents, timelineEvents, parseDate, iso, startOfWeek, addDays, eventAccessibleName, publicationStatus, shareToken} from './calendar.js';
const event = (id, start, end) => ({id, displayDate: '2026-10-02', displayEndDate: '2026-10-02', startMinute: start, endMinute: end});
test('adjacent events keep one lane and overlapping events share lanes', () => {
  const adjacent = timelineEvents([event('a', 540, 600), event('b', 600, 660)], '2026-10-02');
  assert.deepEqual(adjacent.map(e => e.lanes), [1, 1]);
  const overlapping = timelineEvents([event('a', 540, 600), event('b', 570, 630), event('c', 630, 660)], '2026-10-02');
  assert.deepEqual(overlapping.map(e => e.lanes), [2, 2, 1]);
  assert.deepEqual(overlapping.map(e => e.lane), [0, 1, 0]);
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
