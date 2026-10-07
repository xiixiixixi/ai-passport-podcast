/* Shared playback arithmetic. No network or DOM side effects. */
(function (root, factory) {
  const value = factory();
  if (typeof module === 'object' && module.exports) module.exports = value;
  else root.PodcastCore = value;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  'use strict';
  class ListeningMeter {
    constructor() { this.total = 0; this.previous = null; }
    reset() { this.total = 0; this.previous = null; }
    breakContinuity() { this.previous = null; }
    sample(now, position, audible, rate = 1, seeking = false) {
      const last = this.previous;
      this.previous = audible && !seeking && Number.isFinite(now) && Number.isFinite(position)
        ? { now, position, rate: Math.max(.25, Number(rate) || 1) } : null;
      if (!last || !this.previous) return this.total;
      const wall = now - last.now;
      const media = (position - last.position) * 1000;
      // Only media movement can support elapsed listening, including background timers.
      if (wall > 0 && media > 0 && media <= wall * Math.max(last.rate, this.previous.rate) + 500) {
        this.total += Math.min(wall, media / Math.max(last.rate, this.previous.rate));
      }
      return this.total;
    }
  }
  function validEvent(event) {
    return event && typeof event.session_id === 'string' && event.session_id.length > 4
      && Number.isSafeInteger(event.seq) && event.seq > 0
      && Number.isFinite(event.position_ms) && event.position_ms >= 0
      && Number.isFinite(event.listened_ms) && event.listened_ms >= 0
      && ['playing', 'paused', 'stopped', 'ended'].includes(event.state);
  }
  class EventOutbox {
    constructor({events = [], send, save = () => true, changed = () => {}, received = () => {}}) {
      this.events = Array.isArray(events) ? events.filter(validEvent) : [];
      this.send = send; this.save = save; this.changed = changed; this.received = received; this.running = null;
    }
    add(event) {
      if (!validEvent(event)) throw new Error('无效收听记录');
      if (!this.events.some(e => e.session_id === event.session_id && e.seq === event.seq)) this.events.push({...event});
      this.persist();
    }
    persist() {
      const durable = this.save(this.events.map(e => ({...e})));
      this.changed({pending: this.events.length, durable});
    }
    drain() {
      if (this.running) return this.running;
      this.running = this.deliver().finally(() => { this.running = null; });
      return this.running;
    }
    async deliver() {
      const failedSessions = new Set();
      while (true) {
        const event = this.events.find(item => !failedSessions.has(item.session_id));
        if (!event) break;
        try {
          const response = await this.send({...event});
          const index = this.events.findIndex(e => e.session_id === event.session_id && e.seq === event.seq);
          if (index >= 0) this.events.splice(index, 1);
          this.received(response, event);
          this.persist();
        } catch (error) {
          failedSessions.add(event.session_id);
          this.changed({pending: this.events.length, durable: true, error});
        }
      }
    }
  }
  function clock(seconds) {
    const n = Math.max(0, Math.floor(Number(seconds) || 0));
    return n >= 3600 ? `${Math.floor(n / 3600)}:${String(Math.floor(n / 60) % 60).padStart(2, '0')}:${String(n % 60).padStart(2, '0')}`
      : `${Math.floor(n / 60)}:${String(n % 60).padStart(2, '0')}`;
  }
  function listening(milliseconds) {
    const minutes = Math.floor(Math.max(0, Number(milliseconds) || 0) / 60000);
    if (minutes === 0) return (Number(milliseconds) || 0) > 0 ? '不到 1 分钟' : '0 分钟';
    return minutes >= 60 ? `${Math.floor(minutes / 60)} 小时${minutes % 60 ? ` ${minutes % 60} 分钟` : ''}` : `${minutes} 分钟`;
  }
  function status(progress) {
    const state = progress?.status;
    return state === 'completed' ? '已听完' : state === 'in_progress' ? '听了一部分' : '未听过';
  }
  return { ListeningMeter, EventOutbox, clock, listening, status, validEvent };
});
