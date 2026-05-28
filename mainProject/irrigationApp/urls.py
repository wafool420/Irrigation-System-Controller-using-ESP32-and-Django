from django.urls import path
from . import views

urlpatterns = [
    path("", views.dashboard, name="dashboard"),
    path("api/command/", views.esp32_command_api, name="esp32_command_api"),
    path("api/device-status/", views.device_status_api, name="device_status_api"),
]