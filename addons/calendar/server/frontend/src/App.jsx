import {useEffect, useLayoutEffect, useRef, useState} from 'react';
import {iso, parseDate, dateAt, addDays, rangeStart, dayEvents, timelineEvents, timeLabel, colour, eventClockTimes, eventTime, eventAccessibleName, publicationStatus, shareToken, eventKey, currentEvent, refreshCalendar} from './calendar.js';

const format = (date, options) => date.toLocaleDateString('en-GB', options);
const initialData = () => ({events: [], fields: [], preferences: {firstDay: 1, timeZone: ''}, range: null});

function DatePicker({selected, firstDay, range, choose, close}) {
  const panel = useRef(null);
  const [month, setMonth] = useState(selected.getMonth());
  const [year, setYear] = useState(String(selected.getFullYear()));
  const start = rangeStart(dateAt(Math.max(1, Math.min(9999, Number(year) || selected.getFullYear())), month, 1), 2, firstDay);
  useEffect(() => {
    const outside = e => {if (!panel.current.parentElement.contains(e.target)) close();};
    const escape = e => {if (e.key === 'Escape') close();};
    document.addEventListener('mousedown', outside); document.addEventListener('keydown', escape);
    return () => {document.removeEventListener('mousedown', outside); document.removeEventListener('keydown', escape);};
  }, [close]);
  return <div className="date-picker" ref={panel}>
    <div className="picker-header"><select aria-label="Month" value={month} onChange={e => setMonth(Number(e.target.value))}>{Array.from({length:12}, (_, i) => <option value={i} key={i}>{format(dateAt(2000, i, 1), {month:'long'})}</option>)}</select>
      <input aria-label="Year" type="number" min="1" max="9999" value={year} onChange={e => setYear(e.target.value)} /></div>
    <div className="picker-days">{Array.from({length:42}, (_, i) => {
      const day = addDays(start, i), key = iso(day);
      return <button key={key} aria-label={format(day, {day:'numeric',month:'long',year:'numeric'})} disabled={range && (key < range.from || key > range.through)} className={day.getMonth() === month ? '' : 'other-month'} onClick={() => choose(day)}>{day.getDate()}</button>;
    })}</div>
  </div>;
}

function Month({start, selected, events, fields, range, open, chooseDay}) {
  const named = fields.includes('title'), EventBox = fields.length ? 'button' : 'div';
  const grid = useRef(null);
  const [capacity, setCapacity] = useState(2);
  useLayoutEffect(() => {
    const observer = new ResizeObserver(() => setCapacity(Math.max(0, Math.floor((grid.current.clientHeight / 6 - 54) / 25))));
    observer.observe(grid.current); return () => observer.disconnect();
  }, []);
  return <div className="month">
    <div className="weekdays">{Array.from({length: 7}, (_, i) => <div key={i}>{format(addDays(start, i), {weekday: 'short'})}</div>)}</div>
    <div className="month-grid" ref={grid}>{Array.from({length: 42}, (_, i) => {
      const day = addDays(start, i), key = iso(day), entries = dayEvents(events, key);
      const allowed = !range || (key >= range.from && key <= range.through);
      return <div className={`month-day ${key === iso(new Date()) ? 'today' : ''} ${allowed ? '' : 'outside-range'}`} key={key}>
        <div className={`day-number ${key === iso(new Date()) ? 'today' : ''} ${day.getMonth() !== selected.getMonth() ? 'other-month' : ''}`}>{day.getDate()}</div>
        {entries.slice(0, capacity).map(e => <EventBox className="month-event" key={eventKey(e)} onClick={fields.length ? () => open(e) : undefined} style={{'--event-colour': colour(named ? e.colour : '')}} aria-label={eventAccessibleName(e, fields)} title={named ? `${e.title || ''}\n${eventTime(e)}` : undefined}>{named && <>{e.allDay ? '' : `${timeLabel(e.startMinute)} `}{e.title}</>}</EventBox>)}
        {entries.length > capacity && <button className="more" aria-label={`+${entries.length - capacity} more`} onClick={() => chooseDay(day)}>+{entries.length - capacity}<span className="more-label"> more</span></button>}
      </div>;
    })}</div>
  </div>;
}

function Timeline({start, count, events, fields, overlap, open}) {
  const named = fields.includes('title'), EventBox = fields.length ? 'button' : 'div';
  const scroll = useRef(null);
  useLayoutEffect(() => {scroll.current.scrollTop = 8 * 60;}, []);
  return <div className="timeline-view"><div className="timeline-scroll" ref={scroll}><div className="timeline-content" style={{'--day-count': count}}>
    <div className="timeline-head" style={{gridTemplateColumns: `56px repeat(${count}, minmax(0, 1fr))`}}><div />{Array.from({length: count}, (_, i) => <div key={i}>{format(addDays(start, i), {weekday: 'short', day: 'numeric'})}</div>)}</div>
    <div className="time-grid">
      {Array.from({length: 24}, (_, hour) => <div className="hour" key={hour} style={{top: hour * 60}}><span>{String(hour).padStart(2, '0')}:00</span><div /></div>)}
      <div className="day-columns" style={{gridTemplateColumns: `repeat(${count}, minmax(0, 1fr))`}}>{Array.from({length: count}, (_, i) => <div className={`day-column ${iso(addDays(start, i)) === iso(new Date()) ? 'today' : ''}`} key={i}>{timelineEvents(events, iso(addDays(start, i))).map(e => {
        const laneWidth = 100 / e.lanes, left = e.lane * laneWidth;
        return <EventBox key={eventKey(e)} className="time-event" onClick={fields.length ? () => open(e) : undefined} aria-label={eventAccessibleName(e, fields)} title={named ? `${e.title || ''}\n${eventTime(e)}` : undefined} style={{top: e.topMinute, height: Math.max(1, e.bottomMinute - e.topMinute - 1), left: `calc(${left}% + 3px)`, width: `calc(${Math.min(100 - left, laneWidth * (e.span + (overlap ? .35 : 0)))}% - 8px)`, zIndex: e.lane, '--event-colour': colour(named ? e.colour : ''), '--event-opacity': overlap ? '32%' : '22%'}}>
          {named && <span className="event-text"><span>{e.title}</span>{e.bottomMinute - e.topMinute >= 44 && <small>{eventTime(e)}</small>}</span>}
        </EventBox>;
      })}</div>)}</div>
    </div></div></div>
  </div>;
}

function EventDetails({event, fields, close}) {
  const dialog = useRef(null);
  useEffect(() => {dialog.current.showModal();}, []);
  const start = event.start.slice(0, 10), end = event.allDay ? iso(addDays(parseDate(event.end.slice(0, 10)), -1)) : event.end.slice(0, 10);
  const [startTime, endTime] = event.allDay ? [] : eventClockTimes(event);
  return <dialog ref={dialog} className="event-dialog" aria-labelledby={fields.includes('title') && event.title ? 'shared-event-title' : 'shared-event-heading'} onClose={close}>
    <button className="close" aria-label="Close" onClick={() => dialog.current.close()}>×</button>
    <div className="event-heading" id="shared-event-heading">Event</div>
    {fields.includes('title') && <div className="event-title" id="shared-event-title">{event.title}</div>}
    <div className="event-details">
      {fields.includes('location') && <><span>Location</span><div>{event.location || ''}</div></>}
      <span>All day</span><div>{event.allDay ? 'Yes' : 'No'}</div>
      <span>Starts</span><div>{start}{event.allDay ? '' : ` ${startTime}`}</div>
      <span>Ends</span><div>{end}{event.allDay ? '' : ` ${endTime}`}</div>
      <span>Time zone</span><div>{event.timeZone}</div>
      {fields.includes('guests') && <><span>Guests</span><div>{(event.guests || []).map(g => <div key={g.email}>{g.name ? `${g.name} (${g.email})` : g.email}</div>)}</div></>}
      {fields.includes('url') && (event.urls?.length || event.url) && <><span>Link</span><div>{(event.urls || [event.url]).map(url => <div key={url}><a href={url} target="_blank" rel="noreferrer">{url}</a></div>)}</div></>}
      {fields.includes('description') && <><span>Notes</span><div className="notes">{event.description || ''}</div></>}
    </div>
  </dialog>;
}

export default function App() {
  const [token, setToken] = useState(() => shareToken(location.pathname, location.hash));
  const [selected, setSelected] = useState(new Date());
  const [mode, setMode] = useState(2);
  const [data, setData] = useState(initialData);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(Boolean(token));
  const [openEventKey, setOpenEventKey] = useState(null);
  const event = currentEvent(data.events, openEventKey);
  const openEvent = event => setOpenEventKey(eventKey(event));
  const [pickDate, setPickDate] = useState(false);
  const dateButton = useRef(null);
  const initialized = useRef(false);
  const activeRequest = useRef(null);
  useEffect(() => {
    const changed = () => {
      const next = shareToken(location.pathname, location.hash);
      activeRequest.current?.abort();
      initialized.current = false;
      setToken(next); setData(initialData()); setOpenEventKey(null); setPickDate(false);
      setSelected(new Date()); setMode(2); setError(''); setLoading(Boolean(next));
    };
    window.addEventListener('hashchange', changed);
    return () => window.removeEventListener('hashchange', changed);
  }, []);
  const start = rangeStart(selected, mode, data.preferences.firstDay);
  const count = mode === 0 ? 1 : mode === 1 ? 7 : 42;
  const from = iso(start), through = iso(addDays(start, count));
  useEffect(() => {
    if (!token) return;
    const controller = new AbortController();
    activeRequest.current = controller;
    async function load(signal) {
      try {
        const response = await fetch(`/api/calendars/${token}?start=${from}&end=${through}`, {signal, cache: 'no-store'});
        if (!response.ok) throw new Error(response.status === 404 ? 'This share link is unavailable.' : 'Unable to load the shared calendar.');
        const next = await response.json();
        if (signal.aborted) throw signal.reason;
        setData(next); setError('');
        setOpenEventKey(key => currentEvent(next.events, key) ? key : null);
        if (!initialized.current) {
          initialized.current = true; setMode(next.preferences.view ?? 2);
          const today = iso(new Date()); setSelected(parseDate(today < next.range.from ? next.range.from : today > next.range.through ? next.range.through : today));
        }
      } catch (e) {
        if (!controller.signal.aborted) {
          setError(signal.reason?.message || e.message);
          setData(previous => ({...previous, events:[]}));
          setOpenEventKey(null);
        }
      } finally {if (!controller.signal.aborted) setLoading(false);}
    }
    setLoading(true);
    const stopRefresh = refreshCalendar(load, controller.signal);
    return () => {controller.abort(); stopRefresh(); if (activeRequest.current === controller) activeRequest.current = null;};
  }, [token, from, through]);
  function period(direction) {return mode === 2 ? dateAt(selected.getFullYear(), selected.getMonth() + direction, 1) : addDays(selected, direction * (mode === 1 ? 7 : 1));}
  function allowedPeriod(date) {
    if (!data.range) return true;
    const begin = rangeStart(date, mode, data.preferences.firstDay);
    return iso(begin) <= data.range.through && iso(addDays(begin, count - 1)) >= data.range.from;
  }
  function closeDatePicker() {
    setPickDate(false);
    if (pickDate) dateButton.current?.focus();
  }
  function choose(date) {
    const key = iso(date);
    setSelected(parseDate(data.range ? key < data.range.from ? data.range.from : key > data.range.through ? data.range.through : key : key));
    closeDatePicker();
  }
  if (!token) return <main><p>Open a schedule share link from Pilon.</p></main>;
  return <main className="calendar-page">
    <div className="calendar-toolbar">
      <button onClick={() => choose(new Date())}>Today</button>
      <button className="nav previous" aria-label="Previous period" disabled={!allowedPeriod(period(-1))} onClick={() => setSelected(period(-1))}><span /></button>
      <button className="nav next" aria-label="Next period" disabled={!allowedPeriod(period(1))} onClick={() => setSelected(period(1))}><span /></button>
      <div className="date-picker-anchor"><button ref={dateButton} onClick={() => pickDate ? closeDatePicker() : setPickDate(true)}>{format(selected, mode === 0 ? {day: 'numeric', month: 'long', year: 'numeric'} : {month: 'long', year: 'numeric'})}</button>
        {pickDate && <DatePicker selected={selected} firstDay={data.preferences.firstDay} range={data.range} choose={choose} close={closeDatePicker} />}
      </div>
      <select aria-label="Calendar view" value={mode} onChange={e => {setMode(Number(e.target.value)); setOpenEventKey(null);}}><option value="0">Day</option><option value="1">Week</option><option value="2">Month</option></select>
    </div>
    <div className="calendar-status" role="status" aria-live="polite" aria-atomic="true">{error || (loading ? 'Loading shared calendar…' : <>{data.preferences.timeZone}<br />{publicationStatus(data.publishedAt)}</>)}</div>
    <div className="calendar-scroll">
    {mode === 2 ? <Month start={start} selected={selected} events={data.events} fields={data.fields} range={data.range} open={openEvent} chooseDay={day => {setSelected(day); setMode(0);}} />
      : <Timeline start={start} count={count} events={data.events} fields={data.fields} overlap={data.fields.includes('title') && data.preferences.eventLayout === 'overlap'} open={openEvent} />}
    </div>
    {event && <EventDetails key={eventKey(event)} event={event} fields={data.fields} close={() => setOpenEventKey(null)} />}
  </main>;
}
