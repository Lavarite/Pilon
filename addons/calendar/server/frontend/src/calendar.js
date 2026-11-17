// CalendarPage.qml's date, segment, and overlap layout rules.
export const shareToken = (pathname, hash) => /^\/calendar\/?$/.test(pathname) && /^#[A-Za-z0-9_-]{43}$/.test(hash) ? hash.slice(1) : null;
export const iso = d => `${String(d.getFullYear()).padStart(4, '0')}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
export const dateAt = (year, month, day) => {const d = new Date(2000, 0, 1, 12); d.setFullYear(year, month, day); return d;};
export const parseDate = value => {const [y, m, d] = value.split('-').map(Number); return dateAt(y, m - 1, d);};
export const addDays = (d, n) => dateAt(d.getFullYear(), d.getMonth(), d.getDate() + n);
export const startOfWeek = (d, first = 1) => addDays(d, -(((d.getDay() + 6) % 7 - (first - 1) + 7) % 7));
export const rangeStart = (d, mode, first) => mode === 0 ? d : mode === 1 ? startOfWeek(d, first) : startOfWeek(dateAt(d.getFullYear(), d.getMonth(), 1), first);
export const timeLabel = minute => `${String(Math.floor(minute / 60)).padStart(2, '0')}:${String(minute % 60).padStart(2, '0')}`;
export const eventTime = event => event.allDay ? 'All day' : `${timeLabel(event.segmentStart ?? event.startMinute)}–${timeLabel(event.segmentEnd ?? event.endMinute)}`;
export const eventAccessibleName = (event, fields) => `${fields.includes('title') && event.title ? `${event.title}, ` : ''}${event.displayDate} ${eventTime(event)}`;
export function publicationStatus(value) {
  const published = value ? new Date(value) : null;
  return published && Number.isFinite(published.getTime())
    ? `Last published ${published.toLocaleString('en-GB', {timeZone: 'UTC', timeZoneName: 'short'})}`
    : 'Publication time unavailable.';
}
export const dayEvents = (events, date) => events.filter(e => e.displayDate <= date && (e.displayEndDate > date || (e.displayEndDate === date && e.endMinute > 0)));
export function timelineEvents(events, date) {
  const items = dayEvents(events, date).map(event => {
    const e = {...event};
    e.segmentStart = e.allDay || e.displayDate < date ? 0 : e.startMinute;
    e.segmentEnd = e.allDay || e.displayEndDate > date ? 1440 : e.endMinute;
    e.topMinute = Math.min(e.segmentStart, 1418);
    e.bottomMinute = Math.min(1440, Math.max(e.topMinute + 22, e.segmentEnd));
    return e;
  }).sort((a, b) => a.topMinute - b.topMinute || b.bottomMinute - a.bottomMinute || a.id.localeCompare(b.id));
  let group = [], groupEnd = -1;
  const intersects = (a, b) => a.topMinute < b.bottomMinute && b.topMinute < a.bottomMinute;
  function assign() {
    const ends = [];
    for (const event of group) {
      let lane = ends.findIndex(end => end <= event.topMinute);
      if (lane < 0) lane = ends.length;
      ends[lane] = event.bottomMinute;
      event.lane = lane;
    }
    for (const event of group) {
      event.lanes = ends.length;
      event.span = 1;
      for (let lane = event.lane + 1; lane < ends.length; lane++) {
        if (group.some(other => other.lane === lane && intersects(event, other))) break;
        event.span++;
      }
    }
  }
  for (const event of items) {
    if (event.topMinute >= groupEnd && group.length) {assign(); group = []; groupEnd = -1;}
    group.push(event); groupEnd = Math.max(groupEnd, event.bottomMinute);
  }
  assign();
  return items;
}
export function colour(value) {
  const index = /^theme:([0-5])$/.exec(value || '');
  if (index) {
    // CalendarPalette's swatches for Pilon's default blue accent and dark background.
    const hue = ((217 / 360 + [0, .08, -.08, .2, -.2, .5][Number(index[1])]) % 1 + 1) % 1;
    return `hsl(${hue * 360} 65% 66%)`;
  }
  if (/^#[\da-f]{8}$/i.test(value || '')) return '#' + value.slice(3) + value.slice(1, 3);
  return /^#([\da-f]{3}|[\da-f]{6})$/i.test(value || '') ? value : '#3b82f6';
}
