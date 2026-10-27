#!/usr/bin/env python3
"""Package only the launcher, open runtime and compiler; never game output."""
from pathlib import Path
import hashlib
import shutil
import tarfile
ROOT=Path(__file__).resolve().parents[3]
NAME='ShadowOfTheColossusPC'
VERSION='0.2.1'
OUT=ROOT / 'dist' / NAME
if OUT.exists():shutil.rmtree(OUT)
shutil.copytree(ROOT / 'artifacts/frozen' / NAME,OUT)
shutil.copytree(ROOT / 'artifacts/compiler',OUT / 'compiler')
shutil.copy2(ROOT / 'LICENSE',OUT / 'LICENSE')
shutil.copy2(ROOT / 'docs/SHADERS.md',OUT / 'SHADERS.md')
shutil.copytree(ROOT / 'artifacts/distribution-licenses',OUT / 'licenses')
shutil.copy2(ROOT / 'artifacts/third-party-sources.tar.gz',OUT / 'third-party-sources.tar.gz')
(OUT / 'LEIA-ME.txt').write_text('''Shadow of the Colossus PC — Linux x86_64

Abra ShadowOfTheColossusPC e escolha seu disco ISO/7Z/ZIP/RAR USA SCUS-97472.
A primeira preparação recompila o jogo localmente, com até duas tarefas.
A preparação pode demorar; a janela mostra o progresso. Depois é só jogar.
O pacote contém o compilador. Não é necessário instalar Docker ou um SDK.

Requisitos: Linux x86_64 com glibc 2.39+ (Ubuntu 24.04+, Fedora 40+), OpenGL 3.3,
8 GB de memória e pelo menos 15 GB livres para importação e compilação.

WASD: analógico esquerdo. H/J/Y/U: ✕/○/□/△. I/O: L1/L2. K/L: R1/R2.
Z/X: Start/Select. F1: configurações. Controle USB/Bluetooth também funciona.
F11: alterna tela cheia na resolução do monitor e janela.
Na aba Controles, "Configurar meu controle" associa botoes e analogicos de
controles sem perfil automatico. O perfil e salvo e usado tambem no menu F1.

Na aba Gráficos, escolha Original 4:3 (faixas laterais) ou Esticar (tela toda).
Importe fragment shaders GLSL 330 (.frag/.fs/.glsl) ou use Nitidez HD.
Shaders são validados no driver local; mudanças valem na próxima abertura.
São filtros da imagem final, não pacotes de texturas HD. Desativar remove o filtro.

Dados e saves: ~/.local/share/ShadowOfTheColossusPC (ou XDG_DATA_HOME).
Os arquivos originais não são alterados. Nenhum dado é enviado para a internet.
No painel Sobre é possível adicionar o jogo ao menu de aplicativos.

Base nativa: HenryKun55/celosia e ran-j/PS2Recomp (GPL-3.0).
Launcher Linux: GenissonEmilio; baseado nos projetos ForbiddenMemoriesPC e
CrashBandicootPC. O código correspondente do port está em compiler/source.
Licenças: licenses/; versões/fontes: licenses/*/source-packages.tsv.
Dependências estáticas do recompilador: third-party-sources.tar.gz.
O runtime e os scripts de build estão em compiler/source.
GCC 13 e bibliotecas Ubuntu: fontes em https://archive.ubuntu.com/ubuntu/pool/
(identificadas pelos pacotes em packages.txt); GCC em https://gcc.gnu.org/gcc-13/.

Shadow of the Colossus pertence a Sony / Team Ico. Projeto independente.
''',encoding='utf-8')
for p in OUT.rglob('*'):
    if p.is_file() and (p.suffix.lower() in ('.iso','.7z','.rar','.xff','.elf','.ps2') or p.name in ('NICO.DAT','SCUS_974.72') or p.name.startswith('func_') or p.name=='complete.json'):
        raise ValueError('Unexpected game-derived file in package: '+str(p))
archive=ROOT / 'dist' / f'{NAME}-{VERSION}-linux-x86_64.tar.gz'
with tarfile.open(archive,'w:gz',compresslevel=6) as t:t.add(OUT,arcname=NAME)
sha=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_suffix(archive.suffix+'.sha256').write_text(sha+'  '+archive.name+'\n')
print(archive,sha)
