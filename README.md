🔐 Sistema de Control RFID con Arduino

Lectura de llaves y tags RFID con monitoreo web, estadísticas y administración de registros

Este proyecto implementa un sistema de identificación mediante RFID integrado con una plataforma web para la visualización, control y administración de tags registrados. Combina hardware Arduino con comunicación en red y un panel HTML interactivo para monitorear en tiempo real la actividad del sistema.

🚀 Características principales
🔧 Módulo físico (Arduino)

Lectura de tags y llaves RFID (MFRC522 / RC522).

Comunicación con el servidor mediante protocolo serial o red (Ethernet/WiFi según versión).

Registro automático de accesos y eventos.

Estructura modular que permite añadir nuevos tipos de tarjetas.

🌐 Panel web (HTML + JS + CSS)

Un dashboard visual donde el usuario puede:

Ver el total de tags registrados.

Mostrar estadísticas y gráficas dinámicas (ej. actividad por hora, tags más usados, eventos recientes).

Listar todos los tags almacenados.

Eliminar o desactivar tags directamente desde la interfaz.

Ver accesos en tiempo real si el Arduino está enviando eventos constantemente.

🗄️ Backend / Servidor

API simple para:

recibir lecturas del Arduino

registrar nuevos tags

eliminar o modificar registros

enviar datos al panel web

Almacenamiento en archivo local o base de datos (según configuración del proyecto).

🧠 Objetivo del proyecto

Crear un sistema completo de:

Identificación

Control de acceso

Monitoreo

Gestión de usuarios RFID

mezclando hardware físico (Arduino), redes y un panel web intuitivo.

Ideal para:

laboratorios

control de inventario

acceso a salas

proyectos educativos de IoT y redes

⚙️ Tecnologías utilizadas
Hardware

Arduino UNO / Mega / ESP32

Módulo RFID MFRC522

Módulo WiFi/Ethernet (opcional)

Software

Arduino IDE (C/C++)

HTML + CSS + JavaScript

Servidor local / API para administración

Gráficas con Chart.js o similar
