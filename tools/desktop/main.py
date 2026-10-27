#!/usr/bin/env python3
"""Shadow of the Colossus PC — standalone, offline GTK desktop launcher."""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model import Library, VERSION, assets_directory, program_directory
from graphics import Graphics

if len(sys.argv) > 1 and sys.argv[1] == "--pipeline-helper":
    from pipeline import helper
    helper(sys.argv[2], sys.argv[3], sys.argv[4:])
    raise SystemExit(0)

if getattr(sys,"frozen",False):
    os.environ.pop("GTK_MODULES",None)
    os.environ["FONTCONFIG_FILE"]=str(assets_directory() / "fonts.conf")
    # Register the shipped fonts by absolute path. Older fontconfig resolves
    # relative font directories differently on newer Linux desktops.
    import ctypes
    fontconfig = ctypes.CDLL("libfontconfig.so.1")
    fontconfig.FcConfigGetCurrent.restype = ctypes.c_void_p
    fontconfig.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    fontconfig.FcConfigAppFontAddFile.restype = ctypes.c_int
    for font_file in sorted((assets_directory() / "fonts").glob("*.ttf")):
        if not fontconfig.FcConfigAppFontAddFile(fontconfig.FcConfigGetCurrent(), os.fsencode(font_file)):
            print("Could not register bundled font", file=sys.stderr)

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_foreign("cairo")
from gi.repository import Gdk, GLib, Gtk, Pango
import cairo


def style(widget, *names):
    for name in names:
        widget.get_style_context().add_class(name)
    return widget


def label(text, *names, wrap=False):
    widget = style(Gtk.Label(label=text), *names)
    widget.set_xalign(0)
    widget.set_line_wrap(wrap)
    return widget


def box(orientation=Gtk.Orientation.VERTICAL, spacing=0, margin=0, classes=()):
    widget = style(Gtk.Box(orientation=orientation, spacing=spacing), *classes)
    for name in ("top", "bottom", "start", "end"):
        getattr(widget, "set_margin_" + name)(margin)
    return widget


def monument(cr, x, y, scale):
    cr.save(); cr.translate(x,y); cr.scale(scale,scale)
    # Original vector artwork: an ancient stone guardian and a sword of light.
    cr.set_source_rgb(.23,.27,.25)
    cr.move_to(-70,80); cr.line_to(-60,-14); cr.line_to(-38,-43); cr.line_to(-30,-76)
    cr.line_to(28,-76); cr.line_to(42,-43); cr.line_to(64,-14); cr.line_to(76,80); cr.close_path(); cr.fill()
    cr.set_source_rgb(.39,.42,.37)
    for x1,y1,w,h in [(-28,-69,54,29),(-41,-32,82,30),(-48,6,96,25),(-52,39,107,30)]:
        cr.rectangle(x1,y1,w,h); cr.fill()
    cr.set_source_rgb(.43,.87,.78)
    for dx in (-17,17): cr.rectangle(dx-5,-57,10,4); cr.fill()
    cr.set_line_width(2); cr.move_to(0,-27);cr.line_to(-10,-12);cr.line_to(0,4);cr.line_to(10,-12);cr.close_path();cr.stroke()
    cr.set_source_rgb(.76,.7,.53);cr.rectangle(-87,-88,4,155);cr.fill();cr.rectangle(-100,50,30,4);cr.fill()
    cr.set_source_rgb(.65,.87,.8);cr.move_to(-85,-103);cr.line_to(-90,-86);cr.line_to(-80,-86);cr.close_path();cr.fill()
    cr.restore()


class Artwork(Gtk.DrawingArea):
    def __init__(self, small=False):
        super().__init__(); self.small=small
        self.set_size_request(45 if small else 220,55 if small else 220)
        self.connect("draw",self.draw)
        self.get_accessible().set_name("Guardião de pedra e espada luminosa, ilustração decorativa")

    def draw(self, widget, cr):
        w,h=self.get_allocated_width(),self.get_allocated_height()
        if not self.small:
            glow=cairo.RadialGradient(w/2,h/2,5,w/2,h/2,140)
            glow.add_color_stop_rgba(0,.35,.7,.62,.18); glow.add_color_stop_rgba(1,.1,.1,.1,0)
            cr.set_source(glow); cr.paint()
            cr.set_source_rgba(.7,.7,.6,.15)
            for dx in (20,170): cr.rectangle(dx,10,14,195);cr.fill()
        monument(cr,w/2+8,h/2+12,.24 if self.small else .94)
        return False


class Launcher:
    def __init__(self, args):
        self.args = args
        self.library = Library(args.data_dir)
        self.graphics = Graphics(self.library)
        self.busy = False; self.process = None; self.closing = False; self.failed = False
        self.controller_process = None
        provider = Gtk.CssProvider(); provider.load_from_path(str(assets_directory() / "style.css"))
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        settings = Gtk.Settings.get_default()
        settings.set_property("gtk-application-prefer-dark-theme", True)
        self.window = Gtk.Window(title="Shadow of the Colossus PC")
        self.window.set_wmclass("shadow-of-the-colossus-pc", "ShadowOfTheColossusPC")
        self.window.set_default_size(1120, 720); self.window.set_size_request(900, 620)
        self.window.set_position(Gtk.WindowPosition.CENTER)
        self.window.set_icon_from_file(str(assets_directory() / "icon.png"))
        self.window.connect("delete-event", self.close)
        header = Gtk.HeaderBar(); header.set_show_close_button(True)
        header.set_title("SHADOW OF THE COLOSSUS  /  PC EDITION"); self.window.set_titlebar(header)
        root = box(Gtk.Orientation.HORIZONTAL); self.window.add(root)
        sidebar_frame = style(Gtk.Box(),"sidebar")
        sidebar = box(spacing=24, margin=22); sidebar.set_size_request(205,-1)
        sidebar_frame.add(sidebar); root.pack_start(sidebar_frame, False, False, 0)
        brand = box(Gtk.Orientation.HORIZONTAL, 9)
        brand.pack_start(Artwork(True), False, False, 0)
        brand_text = box(spacing=3)
        brand_text.pack_start(label("SHADOW\nOF THE COLOSSUS", "brand"), False,False,0)
        brand_text.pack_start(label("P C   E D I T I O N", "small"),False,False,0)
        brand.pack_start(brand_text,False,False,0); sidebar.pack_start(brand,False,False,0)
        sidebar.pack_start(label("SUA BIBLIOTECA", "eyebrow"),False,False,8)
        self.nav = {}
        self.stack = Gtk.Stack(); self.stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        self.stack.set_hhomogeneous(False); self.stack.set_vhomogeneous(False)
        for key, title in [("home","◈   Início"),("graphics","▧   Gráficos"),("controls","⌘   Controles"),("about","◇   Sobre o port")]:
            button = style(Gtk.Button(label=title),"nav")
            button.get_child().set_xalign(0)
            button.connect("clicked", lambda _, name=key: self.navigate(name))
            sidebar.pack_start(button,False,False,0); self.nav[key] = button
        sidebar.pack_start(Gtk.Box(),True,True,0)
        sidebar.pack_start(label("●  Tudo no seu computador", "small"),False,False,0)
        sidebar.pack_start(label(f"v{VERSION}   •   Linux", "small"),False,False,0)
        body = box(spacing=20,margin=30,classes=("main-content",))
        scroller = Gtk.ScrolledWindow(); scroller.set_policy(Gtk.PolicyType.NEVER,Gtk.PolicyType.AUTOMATIC)
        scroller.add(body); root.pack_start(scroller,True,True,0)
        body.pack_start(label("BIBLIOTECA    /    SHADOW OF THE COLOSSUS", "breadcrumb"),False,False,0)
        body.pack_start(self.stack,True,True,0)
        self.stack.add_named(self.home(),"home")
        self.stack.add_named(self.graphics_page(),"graphics")
        self.stack.add_named(self.controls(),"controls")
        self.stack.add_named(self.about(),"about")
        footer = box(Gtk.Orientation.HORIZONTAL,spacing=15,classes=("footer",)); footer.set_margin_top(2)
        text = label("Atravesse as Terras Proibidas.","small")
        footer.pack_start(text,True,True,0)
        saves = style(Gtk.Button(label="Meus saves ↗"),"link"); saves.connect("clicked",self.open_saves)
        footer.pack_end(saves,False,False,0); body.pack_end(footer,False,False,0)
        self.window.show_all(); self.navigate(args.page)
        self.progress.hide()
        if self.library.disc:
            self.set_ready()
        else:
            self.play_button.set_sensitive(False)
            self.status.set_text("Escolha sua ROM para começar.")
        target = Gtk.TargetEntry.new("text/uri-list",0,0)
        self.drop.drag_dest_set(Gtk.DestDefaults.ALL,[target],Gdk.DragAction.COPY)
        self.drop.connect("drag-data-received", self.dropped)
        if args.rom:
            GLib.timeout_add(300,lambda: self.prepare(args.rom) or False)
        elif args.test_game:
            GLib.timeout_add(300,self.play)
        elif args.screenshot:
            GLib.timeout_add(700,self.capture_and_exit)

    def home(self):
        page = box(spacing=22)
        hero_outer = style(Gtk.Box(),"hero")
        hero = box(Gtk.Orientation.HORIZONTAL, margin=24)
        hero_outer.add(hero)
        copy = box(spacing=12)
        copy.pack_start(label("DAS TERRAS PROIBIDAS AO SEU PC", "eyebrow"),False,False,0)
        copy.pack_start(label("Shadow of the\nColossus", "hero-title",wrap=True),False,False,0)
        copy.pack_start(label("Uma jornada além do impossível.\nSeu disco. Sua aventura.","hero-subtitle"),False,False,0)
        pills = box(Gtk.Orientation.HORIZONTAL,8)
        for text in ("PORT NATIVO","JOGO ORIGINAL","OFFLINE"):
            pills.pack_start(label(text,"pill"),False,False,0)
        copy.pack_start(pills,False,False,4)
        hero.pack_start(copy,True,True,0); hero.pack_end(Artwork(),False,False,0)
        page.pack_start(hero_outer,False,False,0)
        panel_outer = style(Gtk.Box(orientation=Gtk.Orientation.VERTICAL),"panel")
        panel = box(spacing=17,margin=22); panel_outer.pack_start(panel,True,True,0)
        heading = box(Gtk.Orientation.HORIZONTAL,12)
        heading.pack_start(label("Seu jogo", "panel-title"),True,True,0)
        heading.pack_end(label("01  /  ESCOLHA · PREPARE · JOGUE", "eyebrow"),False,False,0)
        panel.pack_start(heading,False,False,0)
        self.drop = style(Gtk.EventBox(),"drop-zone")
        inner = box(Gtk.Orientation.HORIZONTAL,16,18)
        icon = label("▣","section-title"); inner.pack_start(icon,False,False,5)
        info = box(spacing=6)
        self.file_title = label("Selecione sua ROM", "file-title")
        self.file_title.set_ellipsize(Pango.EllipsizeMode.MIDDLE); self.file_title.set_max_width_chars(38)
        info.pack_start(self.file_title,False,False,0)
        self.file_subtitle = label("ISO, RAR, ZIP ou 7Z · edição americana", "muted")
        info.pack_start(self.file_subtitle,False,False,0)
        inner.pack_start(info,True,True,0)
        self.choose_button = style(Gtk.Button(label="Escolher arquivo…"),"secondary")
        self.choose_button.connect("clicked",self.choose)
        inner.pack_end(self.choose_button,False,False,0); self.drop.add(inner)
        panel.pack_start(self.drop,False,False,0)
        self.progress = Gtk.ProgressBar(); panel.pack_start(self.progress,False,False,0)
        actions = box(Gtk.Orientation.HORIZONTAL,20)
        state = box(spacing=5)
        self.status = label("Escolha sua ROM para começar.","status"); self.status.set_line_wrap(True)
        state.pack_start(self.status,False,False,0)
        state.pack_start(label("Seu arquivo fica apenas neste computador.","muted"),False,False,0)
        actions.pack_start(state,True,True,0)
        self.play_button = style(Gtk.Button(label="▶   Iniciar jogo"),"play")
        self.play_button.set_tooltip_text("Abrir Shadow of the Colossus nativamente no PC")
        self.play_button.connect("clicked",self.play)
        actions.pack_end(self.play_button,False,False,0); panel.pack_start(actions,False,False,0)
        page.pack_start(panel_outer,False,False,0)
        note = label("Dica: você também pode arrastar o arquivo para a área acima.","muted")
        page.pack_start(note,False,False,0)
        page.pack_start(Gtk.Box(),True,True,0)
        return page

    def controls(self):
        page=box(spacing=24)
        page.pack_start(label("Pronto para jogar?", "section-title"),False,False,0)
        page.pack_start(label("Jogue com teclado ou controle USB/Bluetooth.\nH / J / Y / U são os botões ✕ / ○ / □ / △. F1 abre as configurações.\nOs controles originais do jogo estão disponíveis; use F1 para ajustar a imagem e o áudio.","muted",wrap=True),False,False,0)
        self.controller_status = label("Perfis automáticos para controles Xbox, PlayStation e genéricos.\nSe os botões não responderem ou estiverem trocados, configure seu controle.", "muted", wrap=True)
        page.pack_start(self.controller_status, False, False, 0)
        self.controller_button = style(Gtk.Button(label="Configurar meu controle…"), "secondary")
        self.controller_button.connect("clicked", self.configure_controller)
        page.pack_start(self.controller_button, False, False, 0)
        grid=Gtk.Grid(column_spacing=28,row_spacing=15); style(grid,"panel")
        rows=[("W A S D","Mover · analógico esquerdo"),("↑ ↓ ← →","Direcional"),
              ("H / J / Y / U","✕ / ○ / □ / △"),("I / O","L1 / L2"),("K / L","R1 / R2"),
              ("Z / X","Start / Select"),("F1","Menu de configurações"),("F11","Tela cheia")]


        for i,(keys,description) in enumerate(rows):
            a=label(keys,"key"); a.set_margin_start(20); a.set_margin_top(8)
            b=label(description,"muted"); b.set_margin_end(25)
            grid.attach(a,0,i,1,1); grid.attach(b,1,i,1,1)
        page.pack_start(grid,False,False,0); page.pack_start(Gtk.Box(),True,True,0)
        return page

    def configure_controller(self, *_):
        if self.busy or self.process or self.controller_process:
            self.controller_status.set_text("Feche o jogo antes de configurar o controle.")
            return
        tool = self.library.program / "compiler/bin/sotc-input-tool"
        if not tool.is_file():
            self.error("Atualize o pacote", "A ferramenta de configuração não está neste pacote.")
            return
        profile = self.library.directory / "controllers/gamecontrollerdb.txt"
        profile.parent.mkdir(parents=True, exist_ok=True)
        try:
            with (self.library.directory / "last-controller.log").open("w") as log:
                self.controller_process = subprocess.Popen([str(tool), "--output", str(profile)],
                    env=self.library.game_environment(), stdout=log, stderr=subprocess.STDOUT)
        except OSError as exc:
            self.error("Não foi possível configurar o controle", str(exc))
            return
        self.controller_button.set_sensitive(False)
        self.controller_status.set_text("Siga as instruções na janela de configuração.")
        GLib.timeout_add(250, self.watch_controller)

    def watch_controller(self):
        code = self.controller_process.poll()
        if code is None:
            return True
        self.controller_process = None
        self.controller_button.set_sensitive(True)
        self.controller_status.set_text("Perfil salvo. Ele será usado na próxima abertura do jogo." if code == 0
            else "Configuração não concluída. Os perfis anteriores foram preservados.")
        return False

    def graphics_page(self):
        page = box(spacing=20)
        page.pack_start(label("Seu monitor. Sua imagem.", "section-title"),False,False,0)
        page.pack_start(label("Escolha como jogar. As mudanças serão usadas na próxima abertura do jogo.","muted",wrap=True),False,False,0)
        panel = box(spacing=16,classes=("panel","graphics-panel"))
        panel.pack_start(label("Tela e proporção", "panel-title"),False,False,0)
        fullscreen, mode = self.graphics.display()
        self.fullscreen = style(Gtk.ComboBoxText(),"graphics-select")
        for text in ("Janela redimensionável", "Tela cheia · resolução do monitor"):
            self.fullscreen.append_text(text)
        self.fullscreen.set_active(int(fullscreen))
        panel.pack_start(self.fullscreen,False,False,0)
        self.aspect = style(Gtk.ComboBoxText(),"graphics-select")
        for text in ("Original 4:3 · preservar proporção", "Esticar · preencher a tela", "Pixels · escala inteira"):
            self.aspect.append_text(text)
        self.aspect.set_active(mode)
        panel.pack_start(self.aspect,False,False,0)
        self.fullscreen.connect("changed", self.save_graphics)
        self.aspect.connect("changed", self.save_graphics)
        panel.pack_start(label("F11 alterna entre janela e tela cheia durante o jogo.\nOriginal mantém faixas laterais em monitores largos. Esticar ocupa toda a tela.","muted",wrap=True),False,False,0)
        page.pack_start(panel,False,False,0)
        panel = box(spacing=15,classes=("panel","graphics-panel"))
        panel.pack_start(label("Shaders e filtros", "panel-title"),False,False,0)
        panel.pack_start(label("Importe um fragment shader GLSL 330 (.frag, .fs ou .glsl).\nO filtro atua sobre a imagem final; não substitui as texturas do jogo por texturas HD.","muted",wrap=True),False,False,0)
        self.shader_status = label("", "status",wrap=True)
        panel.pack_start(self.shader_status,False,False,0)
        actions = box(Gtk.Orientation.HORIZONTAL,10)
        self.shader_import = style(Gtk.Button(label="Importar shader…"),"secondary")
        self.shader_import.connect("clicked",self.choose_shader)
        actions.pack_start(self.shader_import,False,False,0)
        self.shader_hd = style(Gtk.Button(label="Usar Nitidez HD"),"secondary")
        self.shader_hd.connect("clicked",lambda *_: self.import_shader(assets_directory()/"shaders/nitidez-hd.frag"))
        actions.pack_start(self.shader_hd,False,False,0)
        self.shader_clear = style(Gtk.Button(label="Desativar"),"secondary")
        self.shader_clear.connect("clicked",self.clear_shader)
        actions.pack_start(self.shader_clear,False,False,0)
        panel.pack_start(actions,False,False,0)
        template = style(Gtk.Button(label="Abrir modelo de shader ↗"),"link")
        template.set_halign(Gtk.Align.START); template.connect("clicked",self.open_shader_template)
        panel.pack_start(template,False,False,0)
        self.video_panel = panel
        self.refresh_shader()
        page.pack_start(panel,False,False,0)
        page.pack_start(label("Shaders são copiados para a biblioteca e validados no seu driver.\nDepois da importação, você pode mover o arquivo original.","muted",wrap=True),False,False,0)
        return page

    def save_graphics(self,*_):
        try:
            self.graphics.save_display(self.fullscreen.get_active()==1,self.aspect.get_active())
        except OSError as exc:
            self.error("Não foi possível salvar a imagem",str(exc))

    def refresh_shader(self):
        shader = self.graphics.selected()
        self.shader_status.set_text("●  " + ("Ativo: " + shader.stem.rsplit("-",1)[0] if shader else "Imagem original · sem shader"))
        self.shader_clear.set_sensitive(bool(shader))

    def clear_shader(self,*_):
        try:
            self.graphics.clear(); self.refresh_shader()
        except OSError as exc:
            self.error("Não foi possível desativar o shader",str(exc))

    def choose_shader(self,*_):
        if self.busy or self.process: return
        dialog=Gtk.FileChooserDialog(title="Importar shader GLSL 330",parent=self.window,action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons("Cancelar",Gtk.ResponseType.CANCEL,"Validar e importar",Gtk.ResponseType.OK)
        selected=Gtk.FileFilter(); selected.set_name("Fragment shaders GLSL 330")
        for extension in ("frag","fs","glsl"):
            selected.add_pattern("*."+extension); selected.add_pattern("*."+extension.upper())
        dialog.add_filter(selected)
        path = dialog.get_filename() if dialog.run()==Gtk.ResponseType.OK else None
        dialog.destroy()
        if path: self.import_shader(path)

    def import_shader(self,path):
        if self.busy or self.process: return
        self.busy=True; self.video_panel.set_sensitive(False)
        self.play_button.set_sensitive(False); self.choose_button.set_sensitive(False)
        self.shader_status.set_text("Validando o shader no seu driver…")
        def worker():
            error=None
            try: self.graphics.import_shader(path)
            except Exception as exc: error=str(exc)
            GLib.idle_add(self.shader_imported,error)
        threading.Thread(target=worker,daemon=False).start()

    def shader_imported(self,error):
        self.busy=False; self.video_panel.set_sensitive(True)
        self.play_button.set_sensitive(bool(self.library.disc)); self.choose_button.set_sensitive(True)
        if self.closing: Gtk.main_quit(); return False
        if error:
            self.refresh_shader()
            self.error("Esse shader não foi ativado",error)
        else: self.refresh_shader()
        return False

    def open_shader_template(self,*_):
        try:
            import shutil
            path=self.library.directory/"shaders/modelo.frag"; path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists(): shutil.copy2(assets_directory()/"shaders/modelo.frag",path)
            Gtk.show_uri_on_window(self.window,path.as_uri(),Gdk.CURRENT_TIME)
        except (OSError,GLib.Error) as exc:
            self.error("Não foi possível abrir o modelo",str(exc))

    def about(self):
        page=box(spacing=23)
        page.pack_start(label("Uma nova forma de reviver\nShadow of the Colossus.","section-title"),False,False,0)
        page.pack_start(label("O jogo original, rodando nativamente no Linux, com uma porta de entrada feita para o seu PC.\n\nEscolha seu disco uma vez. Nas próximas sessões, é só clicar em jogar.","hero-subtitle",wrap=True),False,False,0)
        page.pack_start(label("SUA ROM", "eyebrow"),False,False,0)
        page.pack_start(label("Compatível com a edição americana SCUS-97472. ISO e RAR/ZIP/7Z são preparados na pasta do aplicativo. A primeira preparação recompila seu disco localmente; depois o cache é reutilizado. Sua ROM original não é alterada.","muted",wrap=True),False,False,0)
        page.pack_start(label("CRÉDITOS", "eyebrow"),False,False,0)
        page.pack_start(label("Interface: GenissonEmilio, baseada no launcher de ForbiddenMemoriesPC.\nBase nativa: HenryKun55 / Celosia (GPL-3.0).\nRecompilador/runtime: ran-j / PS2Recomp e colaboradores.\nReferência PS2: Tenkaichi3Decomp; launcher: ForbiddenMemoriesPC / CrashBandicootPC.\nShadow of the Colossus pertence a seus respectivos titulares.","muted",wrap=True),False,False,0)
        install=style(Gtk.Button(label="＋  Adicionar ao menu de aplicativos"),"secondary")
        install.set_halign(Gtk.Align.START); install.connect("clicked",self.install_menu)
        page.pack_start(install,False,False,0)
        self.install_status=label("Depois disso, você pode abrir o jogo pelo menu do Linux.","muted",wrap=True)
        page.pack_start(self.install_status,False,False,0)
        page.pack_start(Gtk.Box(),True,True,0)
        return page

    def navigate(self,name):
        self.stack.set_visible_child_name(name)
        for key,button in self.nav.items():
            context=button.get_style_context()
            (context.add_class if key==name else context.remove_class)("active")

    def choose(self,*_):
        dialog=Gtk.FileChooserDialog(title="Escolha sua ROM de Shadow of the Colossus",parent=self.window,
                                     action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons("Cancelar",Gtk.ResponseType.CANCEL,"Selecionar ROM",Gtk.ResponseType.OK)
        selected=Gtk.FileFilter(); selected.set_name("Shadow of the Colossus · ISO, RAR, ZIP e 7Z")
        for extension in ("iso","rar","zip","7z"):
            selected.add_pattern("*."+extension); selected.add_pattern("*."+extension.upper())
        dialog.add_filter(selected)
        if dialog.run()==Gtk.ResponseType.OK:
            path=dialog.get_filename(); dialog.destroy(); self.prepare(path)
        else:
            dialog.destroy()

    def dropped(self,widget,context,x,y,selection,info,time):
        uris=selection.get_uris()
        accepted=False
        if not self.busy and not self.process and len(uris)==1:
            try:
                path,host=GLib.filename_from_uri(uris[0])
                if host and host!="localhost": raise ValueError("Escolha um arquivo deste computador.")
                self.prepare(path); accepted=True
            except (GLib.Error,ValueError) as exc:
                self.error("Confira o arquivo escolhido",str(exc))
        Gtk.drag_finish(context,accepted,False,time)

    def update_status(self,text,error=False):
        self.status.set_text(text)
        context=self.status.get_style_context()
        context.remove_class("ready"); context.remove_class("error")
        if error: context.add_class("error")
        return False

    def set_ready(self):
        self.file_title.set_text(self.library.state.get("name","Shadow of the Colossus"))
        self.file_subtitle.set_text("SCUS-97472  ·  Edição americana validada")
        self.status.set_text("●  Pronto para jogar")
        self.status.get_style_context().add_class("ready")
        self.play_button.set_sensitive(True)
        self.choose_button.set_label("Trocar ROM…")

    def pulse(self):
        if not self.busy: return False
        self.progress.pulse(); return True

    def prepare(self,path):
        if self.busy or self.process: return False
        self.busy=True; self.failed=False
        self.choose_button.set_sensitive(False); self.play_button.set_sensitive(False)
        self.progress.show(); self.update_status("Preparando seu jogo…")
        GLib.timeout_add(80,self.pulse)
        def worker():
            error=None
            try:
                self.library.prepare(path,lambda text: GLib.idle_add(self.update_status,text))
            except Exception as exc:
                error=str(exc)
            GLib.idle_add(self.prepared,error)
        threading.Thread(target=worker,daemon=False).start()
        return False

    def prepared(self,error):
        self.busy=False; self.progress.hide(); self.choose_button.set_sensitive(True)
        if self.closing: Gtk.main_quit(); return False
        if error:
            self.failed=True
            self.update_status("Não foi possível preparar essa ROM.",True)
            self.error("Confira o arquivo escolhido",error)
            self.play_button.set_sensitive(bool(self.library.disc))
        else:
            self.set_ready()
            if self.args.test_game: GLib.idle_add(self.play)
            elif self.args.screenshot: GLib.timeout_add(500,self.capture_and_exit)
        return False

    def play(self,*_):
        if self.busy or self.process or self.controller_process: return False
        self.busy=True; self.play_button.set_sensitive(False); self.choose_button.set_sensitive(False)
        self.update_status("Abrindo sua próxima aventura…")
        self.progress.show(); GLib.timeout_add(80,self.pulse)
        self.fullscreen.set_sensitive(False); self.aspect.set_sensitive(False); self.video_panel.set_sensitive(False)
        def worker():
            try:
                process=self.library.launch(self.args.test_game,lambda text: GLib.idle_add(self.update_status,text))
                GLib.idle_add(self.game_started,process,None)
            except Exception as exc:
                GLib.idle_add(self.game_started,None,str(exc))
        threading.Thread(target=worker,daemon=False).start()
        return False

    def game_started(self,process,error):
        self.busy=False; self.progress.hide()
        if self.closing:
            Gtk.main_quit(); return False
        if error:
            self.failed=True; self.choose_button.set_sensitive(True); self.play_button.set_sensitive(True)
            self.fullscreen.set_sensitive(True); self.aspect.set_sensitive(True); self.video_panel.set_sensitive(True)
            self.update_status("Não foi possível abrir o jogo.",True); self.error("O jogo não abriu",error)
        else:
            self.process=process; self.play_button.set_label("Em jogo…")
            self.update_status("●  Aventura em andamento. Divirta-se!")
            GLib.timeout_add(300,self.watch_game)
        return False

    def watch_game(self):
        code=self.process.poll()
        if code is None: return True
        self.process=None; self.choose_button.set_sensitive(True)
        self.fullscreen.set_sensitive(True); self.aspect.set_sensitive(True); self.video_panel.set_sensitive(True)
        self.play_button.set_label("▶   Iniciar jogo"); self.set_ready()
        fullscreen, mode = self.graphics.display()
        self.fullscreen.set_active(int(fullscreen)); self.aspect.set_active(mode)
        if code:
            self.failed=True
            self.update_status("O jogo encerrou com um erro.",True)
            self.error("O jogo foi encerrado",f"O jogo retornou o código {code}.\n\nOs detalhes estão em:\n{self.library.directory / 'last-game.log'}")
        elif self.args.test_game:
            directory = self.library.directory / "game-test"
            captures = sorted(directory.glob("frame_*.png"))
            log = (self.library.directory / "last-game.log").read_text(errors="replace")
            result = {"scope":"boot and native renderer; campaign not covered",
                      "native_opengl":"[native] OpenGL renderer up" in log,
                      "gamecore":"GAMECORE entered" in log, "captures":len(captures)}
            result["passed"] = result["native_opengl"] and result["gamecore"] and bool(captures)
            (directory / "report.json").write_text(json.dumps(result,indent=2)+"\n")
            self.failed |= not result["passed"]
            GLib.timeout_add(500,self.capture_and_exit)
        return False

    def error(self,title,text):
        if self.args.test_game or self.args.screenshot:
            print(title+": "+text,file=sys.stderr)
            if self.args.rom or self.args.test_game:
                GLib.timeout_add(100,self.capture_and_exit)
            return
        dialog=Gtk.MessageDialog(transient_for=self.window,flags=0,message_type=Gtk.MessageType.ERROR,
                                 buttons=Gtk.ButtonsType.CLOSE,text=title)
        dialog.format_secondary_text(text); dialog.run(); dialog.destroy()

    def open_saves(self,*_):
        path=self.library.player_directory / "memcards"; path.mkdir(parents=True,exist_ok=True)
        Gtk.show_uri_on_window(self.window,path.as_uri(),Gdk.CURRENT_TIME)

    def install_menu(self,*_):
        try:
            self.library.install_desktop_entry()
            self.install_status.set_text("✓  Atalho criado! Procure “Shadow of the Colossus PC” no menu de aplicativos.")
        except OSError as exc:
            self.error("Não foi possível criar o atalho",str(exc))

    def capture_and_exit(self):
        if self.args.screenshot:
            # Draw the actual widget tree. GDK's X11 window readback can abort
            # inside Cairo 1.16 under Xvfb; widget rendering works on Wayland too.
            surface=cairo.ImageSurface(cairo.FORMAT_ARGB32,self.window.get_allocated_width(),self.window.get_allocated_height())
            self.window.draw(cairo.Context(surface))
            output=Path(self.args.screenshot); output.parent.mkdir(parents=True,exist_ok=True)
            surface.write_to_png(str(output))
        if self.args.test_game:
            result={"passed":not self.failed,"disc":str(self.library.disc),
                    "frame_created":(self.library.directory / "game-test" / "report.json").is_file()}
            (self.library.directory / "launcher-test.json").write_text(json.dumps(result,indent=2)+"\n")
        Gtk.main_quit(); return False

    def close(self,*_):
        if self.busy:
            self.closing=True; self.window.hide(); return True
        Gtk.main_quit(); return False


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom",type=Path,help=argparse.SUPPRESS)
    parser.add_argument("--data-dir",type=Path,help=argparse.SUPPRESS)
    parser.add_argument("--screenshot",type=Path,help=argparse.SUPPRESS)
    parser.add_argument("--test-game",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--install-shortcut",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--page",choices=("home","graphics","controls","about"),default="home",help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.install_shortcut:
        print(Library(args.data_dir).install_desktop_entry()); return 0
    if args.test_game and not args.rom and not Library(args.data_dir).disc:
        parser.error("--test-game requires an imported disc or --rom")
    launcher=Launcher(args); Gtk.main()
    return int(launcher.failed)


if __name__=="__main__":
    raise SystemExit(main())
