#!/usr/bin/env python3
"""Losslessly encode host-rendered PPMs and build original SVG publication layouts."""
import base64
from pathlib import Path
import struct
import zlib

HERE = Path(__file__).resolve().parent
BLUE = '#0063F2'
BLACK = '#111111'
GRAY = '#5B6472'
FONT = 'Noto Sans SC'


def png_from_ppm(name):
    source = HERE / f'{name}.ppm'
    with source.open('rb') as f:
        assert f.readline() == b'P6\n'
        w, h = map(int, f.readline().split())
        assert (w, h) == (240, 320)
        assert f.readline() == b'255\n'
        rgb = f.read()
    assert len(rgb) == w*h*3
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    result = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
              chunk(b'IDAT', zlib.compress(b''.join(b'\0' + rgb[y*w*3:(y+1)*w*3] for y in range(h)), 9)) +
              chunk(b'IEND', b''))
    (HERE / f'{name}.png').write_bytes(result)
    return base64.b64encode(result).decode('ascii')


def text(x, y, content, size, fill=BLACK, weight=400):
    return f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" fill="{fill}">{content}</text>'


def waveform(x,y,scale=1):
    heights=(24,44,66,44,24)
    return ''.join(f'<rect x="{x+i*20*scale}" y="{y-h*scale/2}" width="{9*scale}" height="{h*scale}" fill="{BLUE}"/>' for i,h in enumerate(heights))


def frame(encoded, x, y, width):
    height=width*4/3
    return (f'<rect x="{x-3}" y="{y-3}" width="{width+6}" height="{height+6}" fill="white" stroke="#CBD5E1" stroke-width="2"/>'
            f'<image x="{x}" y="{y}" width="{width}" height="{height}" image-rendering="optimizeSpeed" href="data:image/png;base64,{encoded}"/>')


def save_svg(name,w,h,elements):
    (HERE / f'{name}.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}"><rect width="{w}" height="{h}" fill="white"/>{elements}</svg>\n')


def poster(encoded, title_one, title_two, section, name):
    elements = (f'<path d="M86 92H1114" stroke="{BLACK}" stroke-width="2"/>' +
        text(86,68,'随身听',26,BLACK,600) + text(948,68,'公开预览版',22,GRAY) +
        text(86,204,title_one,72,BLACK,600) + text(86,300,title_two,72,BLUE,600) +
        text(88,365,'护照播客机 · 为可编程护照设备准备的播客播放器',26,GRAY) +
        frame(encoded,360,442,480) +
        text(86,1177,section,25,BLUE,500) +
        f'<path d="M86 1220H1114" stroke="#CBD5E1" stroke-width="2"/>' +
        text(86,1291,'继续收听',28,BLACK,600) + text(440,1291,'单集列表',28,BLACK,600) + text(794,1291,'睡眠定时',28,BLACK,600) +
        text(86,1342,'保存收听位置',23,GRAY) + text(440,1342,'按节目找内容',23,GRAY) + text(794,1342,'到时间自动暂停',23,GRAY) +
        waveform(88,1456,1) + text(230,1453,'实际界面 · 电脑导出 · 示例节目',22,GRAY) +
        text(230,1493,'原始屏幕尺寸 240 × 320 像素',22,GRAY))
    save_svg(name,1200,1600,elements)


def main():
    player = png_from_ppm('player-native')
    library = png_from_ppm('library-native')
    poster(player,'把播客，','带在身边。','01 / 正在播放','play-cover-3x4')
    poster(library,'节目库，','一眼看清。','02 / 选择节目','library-showcase-3x4')
    elements=(f'<path d="M80 70H1520" stroke="{BLACK}" stroke-width="2"/>' +
        text(80,142,'随身听',62,BLACK,600) +
        text(80,238,'把一段声音，',48,BLACK,500) + text(80,307,'带在身边。',48,BLUE,500) +
        text(80,377,'护照播客机 · 可编程护照设备的播客播放器',25,GRAY) +
        text(104,470,'节目库 · 单集选择',28,BLACK,500) +
        text(104,527,'继续收听 · 进度调整',28,BLACK,500) +
        text(104,584,'连续播放 · 睡眠定时',28,BLACK,500) +
        ''.join(f'<rect x="80" y="{y-14}" width="7" height="7" fill="{BLUE}"/>' for y in (470,527,584)) +
        waveform(82,725,1.5) + text(292,726,'公开预览版',26,BLUE,500) +
        text(80,814,'实际界面 · 电脑导出 · 示例节目',23,GRAY) +
        frame(player,950,120,480))
    save_svg('github-showcase',1600,900,elements)
    print('Generated two lossless 240x320 screen PNGs and three original SVG layouts.')


if __name__=='__main__':
    main()
