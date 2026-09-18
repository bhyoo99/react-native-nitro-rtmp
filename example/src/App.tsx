import { useState } from 'react';

import { INITIAL_SOURCE, type SourceMode } from './env';
import FixtureScreen from './FixtureScreen';
import LiveScreen from './LiveScreen';

/**
 * Two screens: the camera screen and the
 * fixture screen, kept for the simulator and for regression checks.
 */
export default function App() {
  const [mode, setMode] = useState<SourceMode>(INITIAL_SOURCE);
  if (mode === 'fixture') {
    return <FixtureScreen onSwitchToCamera={() => setMode('camera')} />;
  }
  return <LiveScreen onSwitchToFixture={() => setMode('fixture')} />;
}
