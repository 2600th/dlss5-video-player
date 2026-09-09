import argparse, json, os, shutil, struct, subprocess, time, msvcrt, psutil, ctypes, csv
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PACKAGE = ROOT.parents[1] / 'dist' / 'DLSSVideoPlayer-v0.14.1-win64'
PROFILES = ROOT / 'profiles-v2'
FFMPEG = PACKAGE / 'ffmpeg.exe'
FFPROBE = PACKAGE / 'ffprobe.exe'
FLAGS = subprocess.CREATE_NO_WINDOW

class GpuMonitor:
    class Memory(ctypes.Structure):
        _fields_=[('total',ctypes.c_ulonglong),('free',ctypes.c_ulonglong),('used',ctypes.c_ulonglong)]
    class Util(ctypes.Structure):
        _fields_=[('gpu',ctypes.c_uint),('memory',ctypes.c_uint)]
    def __init__(self):
        self.api=ctypes.WinDLL('nvml.dll')
        assert self.api.nvmlInit_v2()==0
        self.handle=ctypes.c_void_p()
        assert self.api.nvmlDeviceGetHandleByIndex_v2(0,ctypes.byref(self.handle))==0
        self.samples=[]
    def sample(self,elapsed):
        memory=self.Memory(); util=self.Util(); temperature=ctypes.c_uint(); power=ctypes.c_uint()
        errors=[self.api.nvmlDeviceGetMemoryInfo(self.handle,ctypes.byref(memory)),self.api.nvmlDeviceGetUtilizationRates(self.handle,ctypes.byref(util)),self.api.nvmlDeviceGetTemperature(self.handle,0,ctypes.byref(temperature)),self.api.nvmlDeviceGetPowerUsage(self.handle,ctypes.byref(power))]
        if any(errors): return
        self.samples.append(dict(elapsed_seconds=elapsed,gpu_util_percent=util.gpu,total_used_mib=memory.used/1048576,temperature_c=temperature.value,power_w=power.value/1000))
    def finish(self,path):
        self.api.nvmlShutdown()
        if self.samples:
            with path.open('w',newline='') as f:
                writer=csv.DictWriter(f,fieldnames=list(self.samples[0]));writer.writeheader();writer.writerows(self.samples)

def prepare(name):
    target = PROFILES / name / 'neural-runtime'
    if target.exists(): return target
    shutil.copytree(PACKAGE / 'neural-runtime', target)
    for helper in ['ffmpeg.exe', 'ffprobe.exe']:
        dest = target.parent / helper
        if not dest.exists(): os.link(PACKAGE / helper, dest)
    with (target / 'ReShade.ini').open('a', encoding='utf-8') as f:
        f.write('\n[RenoDX.DLSS5]\nEnableHooks=2\nNeuralUplift=1\nNREnableUpscaling=0\n')
        if name == 'intensity-zero': f.write('NRIntensity=0\n')
    swaps = {
        'sr-310.9': [('dlss-310.9.0/nvngx_dlss.dll', 'nvngx_dlss.dll')],
        'streamline-2.14': [('streamline-2.14.0.0/' + p.name, p.name) for p in (PACKAGE / 'neural-runtime').glob('sl.*.dll')],
        'nr-SF-v2': [('dlssnr-310.8.SF-v2/nvngx_dlssnr.dll', 'nvngx_dlssnr.dll')],
        'nr-stock-310.8': [('dlssnr-310.8.0/nvngx_dlssnr.dll', 'nvngx_dlssnr.dll')],
        'SF-0.53': [('dlssnr-310.8.SF-v2/nvngx_dlssnr.dll', 'nvngx_dlssnr.dll'), ('renodx-dlss-SF-0.53/renodx-dlss.addon64', 'renodx-dlss.addon64')],
    }
    for source, dest in swaps.get(name, []):
        shutil.copy2(ROOT / 'downloads' / source, target / dest)
    if name == 'SF-0.53':
        # Preserve the baseline addon but remove its loadable extension in this isolated profile.
        (target / 'renodx-dlss5.addon64').rename(target / 'renodx-dlss5.addon64.baseline-disabled')
    return target

def decode(data):
    records = []
    pos = 0
    while pos + 12 <= len(data):
        magic, version, kind, count = struct.unpack_from('<IHHI', data, pos)
        if magic != 0x3152574E or version != 1: raise ValueError('Invalid worker metadata')
        pos += 12
        payload = data[pos:pos+count]
        if len(payload) < count: break
        pos += count
        if kind == 1:
            keys = ['phase','completed_frames','total_frames','bytes','elapsed_ms','remaining_ms']
            records.append(dict(zip(keys, struct.unpack('<IQQQqq', payload))))
        elif kind == 2:
            keys = ['ok','cancelled','encoder','armed_before_capture','upscaling_off','inline_contract','created','evaluated','later_failure','frame_count','duration_100ns','native_evaluations','verified_neural_frames','highest_evaluation','detail_bytes']
            result = dict(zip(keys, struct.unpack_from('<9B7xQqQQQI', payload)))
            result['detail'] = payload[60:60+result['detail_bytes']].decode('utf-16le')
            records.append(result)
    return records

def probe(source):
    data = json.loads(subprocess.check_output([str(FFPROBE), '-v','error','-select_streams','v:0','-show_entries','stream=width,height,avg_frame_rate:format=duration','-of','json',str(source)], creationflags=FLAGS))
    stream = data['streams'][0]
    n,d = map(int, stream['avg_frame_rate'].split('/'))
    return stream['width'],stream['height'],n/d,round(float(data['format']['duration'])*1e7)

def run(profile, source, repeat, timeout):
    runtime = prepare(profile)
    dest = ROOT / 'runs' / f'{source.stem}__{profile}__{repeat}'
    dest.mkdir(parents=True, exist_ok=False)
    w,h,fps,duration = probe(source)
    attempts = []
    for restart in [False, True]:
        metadata = dest / ('metadata-restart.bin' if restart else 'metadata.bin')
        startup = subprocess.STARTUPINFO()
        with metadata.open('wb') as f, (dest / 'stdout.txt').open('ab') as log:
            handle = msvcrt.get_osfhandle(f.fileno())
            os.set_handle_inheritable(handle, True)
            startup.lpAttributeList = {'handle_list':[handle]}
            args = [str(runtime / 'NeuralWorker.exe'), '--neural-worker', '--metadata-handle',str(handle),'--source',str(source),'--staging',str(dest / 'output.mkv'),'--width',str(w),'--height',str(h),'--fps',str(fps),'--duration-100ns',str(duration)]
            if restart: args.append('--configuration-restarted')
            start = time.perf_counter()
            monitor=GpuMonitor()
            sampled_at=-1
            process = subprocess.Popen(args, cwd=runtime, stdout=log, stderr=log, startupinfo=startup, close_fds=True, creationflags=FLAGS)
            timed_out = False
            modules = []
            try:
                while process.poll() is None:
                    if time.perf_counter()-start > timeout: raise subprocess.TimeoutExpired(args,timeout)
                    elapsed=time.perf_counter()-start
                    if elapsed-sampled_at>=0.5: monitor.sample(elapsed);sampled_at=elapsed
                    if not modules:
                        live = decode(metadata.read_bytes())
                        if any(x.get('completed_frames',0) >= 10 for x in live):
                            try: modules = sorted(set(x.path for x in psutil.Process(process.pid).memory_maps()))
                            except psutil.Error: pass
                    time.sleep(0.1)
                code=process.returncode
            except subprocess.TimeoutExpired:
                timed_out = True
                subprocess.run(['taskkill','/PID',str(process.pid),'/T','/F'],capture_output=True,creationflags=FLAGS)
                code = process.wait(timeout=10)
            wall = time.perf_counter() - start
            monitor.finish(dest/'gpu.csv')
        records = decode(metadata.read_bytes())
        attempts.append({'returncode':code,'timed_out':timed_out,'wall_seconds':wall,'records':records,'loaded_modules':modules})
        for filename in ['ReShade.log','ReShade.ini','ReShadePreset.ini']:
            if (runtime / filename).exists(): shutil.copy2(runtime / filename, dest / (('restart-' if restart else '') + filename))
        if code != 75: break
    final = next((x for x in reversed(records) if 'ok' in x), {})
    output = {'profile':profile,'source':str(source),'width':w,'height':h,'fps':fps,'repeat':repeat,'attempts':attempts,'result':final,'wall_seconds':wall,'end_to_end_fps':final.get('frame_count',0)/wall,'output':str(dest/'output.mkv')}
    (dest / 'result.json').write_text(json.dumps(output,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in output.items() if k not in ['attempts','source','output'] }),flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--profiles',nargs='+',default=['baseline'])
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--repeats',type=int,default=1)
    parser.add_argument('--start',type=int,default=1)
    parser.add_argument('--timeout',type=int,default=120)
    args=parser.parse_args()
    for rep in range(args.start,args.start+args.repeats):
        for profile in args.profiles: run(profile,args.source.resolve(),rep,args.timeout)
