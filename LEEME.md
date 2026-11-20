# NovaSave2 0.4.13 para Nintendo Switch 2

Homebrew nativo para crear, verificar y restaurar copias locales de partidas de usuario. La app usa libounce y Ounce Devkit.

## Instalación y uso

1. Cierra completamente el juego desde el menú HOME.
2. Copia la carpeta `switch` del ZIP a la raíz de la microSD. Abre `NovaSave2` en tu entorno homebrew.
3. Selecciona una partida con la cruceta. Pulsa **A** para abrir la advertencia de respaldo o **X** para ver los respaldos de esa partida. Para respaldar, elige **Sí** con la cruceta y pulsa **A Confirmar**. La opción inicial es **No**. **Y** actualiza la lista y **+** cierra la app. Mantén arriba o abajo para recorrer las listas rápidamente.
4. Para restaurar, entra con **X**, selecciona el archivo con **A**, elige el perfil de destino, selecciona **Sí** con la cruceta y pulsa **A Confirmar**. La opción inicial es **No** y **B** permite volver. La app verifica el archivo y guarda una copia de la partida del perfil de destino con el sufijo `-previo` antes de escribir.
5. Cuando la app muestre **Guardado y verificado** para el perfil elegido, sal con **+** y abre el juego con ese perfil. La app reabre la partida tras confirmar la escritura y compara los archivos del respaldo con lo que quedó guardado.

El ejecutable va en `switch/NovaSave2/NovaSave2.nro`. Los nuevos respaldos se guardan en `switch/NovaSave2/backups/<nombre del juego [ID]>/<ID del usuario>/`. Si el sistema no devuelve el nombre del juego, se usa el ID para la carpeta.

La lista muestra el nombre del archivo que está en la microSD. Al resaltarlo, aparece el nombre completo con su extensión y el nombre de la carpeta de primer nivel. Puedes copiar un respaldo `.nsv2` desde la PC a `switch/NovaSave2/backups/` o a una carpeta de juego dentro de esa ruta. NovaSave2 encuentra también las copias de la versión anterior y sigue reconociendo los archivos si les cambias el nombre o los mueves dentro de `backups`. La partida se restaura desde el archivo seleccionado de la microSD. La lista agrupa los archivos por juego. Cada `.nsv2` conserva el identificador del perfil de origen y se verifica con esa identidad antes de escribir en el perfil de destino.

Si aparecen varios perfiles con el mismo nombre, el sufijo del ID permite distinguirlos. Si el menú muestra un código hexadecimal, anótalo junto con el mensaje.

## Importar una carpeta ya extraída

1. Copia la carpeta con los archivos de la partida a `switch/NovaSave2/importar/<ID del juego>/<nombre de la copia>/` en la microSD. El menú principal muestra el ID del juego.
2. Selecciona ese juego y pulsa **X**. Elige la entrada **[CARPETA]**, pulsa **A**, selecciona el perfil de destino y confirma.
3. NovaSave2 crea un `*-importado.nsv2` con los archivos de la carpeta y solicita la confirmación de escritura a la consola. La carpeta original se conserva.

Para un perfil marcado **[Crear partida]**, la app pide a la consola que cree el espacio de guardado con los parámetros del juego. Si la consola lo rechaza o faltan esos parámetros, abre el juego con el perfil elegido, guarda una vez y actualiza con **Y**. La transferencia entre perfiles depende de que el juego acepte esos datos. Para la opción **[CARPETA]** usa archivos ya extraídos y legibles. Los contenedores cifrados requieren un formato y un método de apertura compatibles.

## Borrar respaldos

Desde la lista de respaldos, selecciona el nombre del archivo y pulsa **X**, después **A** para confirmar. Esto borra el archivo de la microSD. Usa **B** para cancelar.

## Desempaquetar y preparar una copia para un perfil

1. Selecciona un archivo `.nsv2` en la lista de respaldos y pulsa **Y Desempaquetar**. Los archivos sueltos se guardan en `switch/NovaSave2/editados/<ID del juego>/<nombre del respaldo>-001/`. La app muestra la ruta y agrega un número distinto para cada extracción.
2. Edita los archivos desde la PC y deja los datos en esa misma carpeta. Conserva los nombres y la estructura que espera el juego. Al volver a la app, selecciona el juego y pulsa **ZR Abrir editados** para ir a la primera carpeta encontrada. **X** sigue mostrando todos los respaldos.
3. Selecciona la carpeta **[EDITADOS]** o un archivo `.nsv2` y pulsa **ZR** para guardar una copia NSV2 para un perfil. Elige el perfil, selecciona **Sí** y pulsa **A Confirmar**. La app crea un `*-perfil.nsv2` en la microSD, verifica su integridad y muestra la ruta.
4. Pulsa **A Restaurar** sobre el respaldo, elige el perfil y confirma. NovaSave2 guarda una copia previa y solicita a la consola que confirme la escritura.

La preparación cambia el perfil asociado al contenedor de NovaSave2 y calcula su suma SHA-256. La confirmación del sistema ocurre al restaurar. El juego puede validar por su cuenta el contenido de la partida.

Las carpetas exportadas por versiones anteriores se detectan en `switch/NovaSave2/editados/<ID del juego>-<ID del perfil>/`. Los archivos `.nsv2` dentro de `editados/` aparecen por su nombre y se asocian al juego por su encabezado.

Los archivos editados podrían provocar errores o pérdida de progreso si el juego no reconoce sus datos. NovaSave2 verifica la integridad del contenedor con SHA-256. La confirmación de escritura depende del servicio de archivos y la validez del contenido depende del juego.

## Nombres de juegos

NovaSave2 lee títulos NACP comprimidos y sin comprimir. Cuando el sistema no devuelve el nombre, prueba su lista local y muestra el ID si sigue sin poder identificar el juego. Puedes definir un nombre en `switch/NovaSave2/titulos.txt` con una línea en formato `0400486024DEC000=The Legend of Zelda: Ocarina of Time` y pulsar **Y** para actualizar. Guarda el texto en UTF-8 sin BOM.

## Si un respaldo no se crea

En el menú de juegos, **A Respaldar** guarda una partida instalada en la consola. Desde **ZR Abrir editados**, elige una carpeta con archivos sueltos y usa **ZR Guardar copia NSV2**. Selecciona el perfil. Si falla la creación, la pantalla muestra el paso, la ruta y el código de la microSD o del archivo de origen. Si la creación termina, aparece **Respaldo creado y verificado** o **Copia NSV2 verificada** con la ruta.

Cada intento genera un archivo `.log` con el mismo nombre base que el `.nsv2`, en la misma carpeta. También se guarda el registro de las copias previas a una restauración o a una ampliación. Si la app todavía no tenía ruta para el respaldo, se interrumpió o no pudo mover el registro, búscalo como `diagnostico-*.log` directamente en `switch/NovaSave2/`. La pantalla muestra **Log:** con la ruta al terminar. Si la microSD no admite escritura, la app avisa.

El registro incluye la operación, el ID del juego, pasos con tiempo transcurrido, rutas y tamaños de archivos, comprobación SHA-256, publicación del respaldo y los códigos disponibles en caso de error. Registra hasta 5000 entradas individuales por intento y sigue anotando los errores y el total. El registro puede mostrar nombres de archivos e identificadores de perfil presentes en las rutas. Puedes adjuntar el `.log` para diagnóstico sin compartir la partida.

## Si falla al copiar en la consola

La restauración normal conserva los archivos idénticos, escribe los bloques que cambiaron y confirma todos los cambios de una vez. Si aparece **2002-6706**, vuelve a seleccionar el mismo respaldo y el perfil correcto. En la pantalla de confirmación, pulsa **X Restaurar por etapas** y después elige **Sí** y pulsa **A Confirmar**. El modo por etapas confirma grupos pequeños de cambios y relee la partida al finalizar. Cada etapa confirmada persiste. Si la operación se interrumpe o la consola rechaza otra etapa, puede quedar una partida parcial. Conserva el respaldo `-previo.nsv2` y mantén cerrado el juego hasta que la app indique **Guardado y verificado**. Si el servicio de archivos devuelve un error, la pantalla muestra el código hexadecimal, la operación y el archivo que falló. La app confirma la escritura únicamente después de completar la copia y conserva el respaldo `-previo.nsv2` en la microSD.

### Si también falla por etapas

Selecciona el respaldo y el perfil. En la pantalla final, pulsa **Y Revisar registro**. La app compara el tamaño actual con el límite que declara el juego. Si hay margen, verás los dos tamaños y podrás confirmar con **A Ampliar registro**. La ampliación es permanente, usa memoria interna y requiere que la consola la autorice. La app guarda una copia `-antes-de-ampliar.nsv2` antes de intentarla y lee de nuevo el tamaño si la consola acepta. Cuando aparezca **Registro verificado**, pulsa **A** para intentar la restauración. Si la app muestra los tamaños sin ofrecer un aumento, conserva ese mensaje para el diagnóstico.

## Formato y alcance

Cada archivo `.nsv2` guarda el ID del juego, el identificador del perfil, los archivos y las carpetas de la partida, y un SHA-256 para detectar cambios o corrupción. NovaSave2 interpreta ese contenedor y conserva el contenido binario de la partida. SHA-256 verifica la integridad del respaldo.

La app monta la partida existente a través del servicio de archivos. Al restaurar, escribe los datos en esa partida y solicita la confirmación del sistema mediante `fsdevCommitDevice`. La restauración requiere autorización de lectura y escritura del entorno y respeta la respuesta que devuelva el servicio. Se admiten copias `.nsv2` y carpetas de archivos extraídos para el mismo juego. Se pueden elegir perfiles locales existentes y perfiles cuyo espacio de guardado se pueda crear con los datos del juego. Los respaldos **[EDITADOS]** y **[MODIFICADO]** se escriben al dar la confirmación. La consola y el juego conservan sus propias comprobaciones del contenido. Los archivos de instalación de juegos, DLC y partidas compartidas por toda la consola quedan fuera de esta versión.

## Compilación

Con devkitPro y Ounce Devkit instalados, instala también `switch-zlib` y habilita `DEVKITPRO`, `DEVKITA64` y las herramientas de devkitPro en `PATH`. Abre la carpeta `codigo-fuente` y ejecuta `make`. El resultado es `NovaSave2.nro`.

La versión incluida se compiló con Ounce Devkit 20261117, libounce `39406be1f25a`, devkitA64 GCC 16.1.0 y switch-tools 1.13.1. Se comprobó el respaldo y la restauración del formato en Linux, incluidas partidas vacías, archivos dañados, rutas inválidas, carpetas renombradas, respaldos de otro usuario o título, copias modificadas y datos NACP comprimidos. La prueba del menú y del acceso a partidas en una Switch 2 física sigue pendiente.

[Formato NACP](https://switchbrew.org/wiki/NACP_Format) · [Catálogo de IDs](https://switchbrew.org/wiki/Switch_2:Title_list/Games) · [Ounce Devkit](https://github.com/ouncebrew/ounce-devkit) · [libounce](https://github.com/ouncebrew/libounce)
